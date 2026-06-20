#include "AlarmService.h"
#include "services/storage/StorageService.h"
#include "services/BufferManager.h"
#include "app/audio/SpeakerPlayback.h"
#include "app/audio/AudioPipelineManager.h"
#include "app/audio/WavPlayer.h"
#include "app/wake_word/WakeWordEngine.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include "hal/Board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <ctime>
#include <cstring>
#include <ArduinoJson.h>

[[maybe_unused]] static const char* TAG = "AlarmSvc";

namespace Services {

AlarmService& AlarmService::getInstance() {
    static AlarmService instance;
    return instance;
}

AlarmService::AlarmService()
    : ReactorTask({
          "alarm_svc",
          ThreadConfig::StackSize::STACK_NORMAL,
          ThreadConfig::Priority::LOW,
          ThreadConfig::CORE_NETWORK,
          COMP::ALARM | COMP::AUDIO
      })
{}

AlarmService::~AlarmService() {
    stopActiveAlarm();
}

bool AlarmService::begin() {
    loadAlarms();
    ESP_LOGI(TAG, "AlarmService operational.");
    return true;
}

void AlarmService::loadAlarms() {
    std::lock_guard<std::mutex> lock(m_alarms_mutex);
    m_alarms.clear();
    
    if (!StorageService::getInstance().fileExists("/sdcard/alarms.json")) {
        ESP_LOGI(TAG, "No alarms.json found on SD card. Creating default empty.");
        return;
    }
    
    std::string content = StorageService::getInstance().readFile("/sdcard/alarms.json");
    if (content.empty()) return;
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, content);
    if (err) {
        ESP_LOGE(TAG, "Failed to parse alarms.json: %s", err.c_str());
        return;
    }
    
    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
        Alarm alarm;
        alarm.id = obj["id"];
        alarm.hour = obj["hour"];
        alarm.minute = obj["minute"];
        strncpy(alarm.tone_file, obj["tone_file"] | "/sdcard/alarms/chime.wav", sizeof(alarm.tone_file) - 1);
        alarm.enabled = obj["enabled"];
        m_alarms.push_back(alarm);
    }
    ESP_LOGI(TAG, "Loaded %zu alarms from SD card", m_alarms.size());
}

void AlarmService::saveAlarms() {
    std::lock_guard<std::mutex> lock(m_alarms_mutex);
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& alarm : m_alarms) {
        JsonObject obj = arr.add<JsonObject>();
        obj["id"] = alarm.id;
        obj["hour"] = alarm.hour;
        obj["minute"] = alarm.minute;
        obj["tone_file"] = alarm.tone_file;
        obj["enabled"] = alarm.enabled;
    }
    std::string content;
    serializeJson(doc, content);
    StorageService::getInstance().writeFile("/sdcard/alarms.json", content.c_str());
}

void AlarmService::addOrUpdateAlarm(const Alarm& alarm) {
    {
        std::lock_guard<std::mutex> lock(m_alarms_mutex);
        bool found = false;
        for (auto& item : m_alarms) {
            if (item.id == alarm.id) {
                item.hour = alarm.hour;
                item.minute = alarm.minute;
                strncpy(item.tone_file, alarm.tone_file, sizeof(item.tone_file) - 1);
                item.enabled = alarm.enabled;
                found = true;
                break;
            }
        }
        if (!found) {
            m_alarms.push_back(alarm);
        }
    }
    saveAlarms();
}

void AlarmService::deleteAlarm(int id) {
    {
        std::lock_guard<std::mutex> lock(m_alarms_mutex);
        for (auto it = m_alarms.begin(); it != m_alarms.end(); ++it) {
            if (it->id == id) {
                m_alarms.erase(it);
                break;
            }
        }
    }
    saveAlarms();
}

std::vector<Alarm> AlarmService::getAlarms() {
    std::lock_guard<std::mutex> lock(m_alarms_mutex);
    return m_alarms;
}

void AlarmService::triggerAlarm(const Alarm& alarm) {
    if (m_playing_alarm) {
        return;
    }
    m_playing_alarm = true;
    strncpy(m_active_tone_file, alarm.tone_file, sizeof(m_active_tone_file) - 1);

    EmbeddedSysDb::getInstance().mutate([alarm](SystemState& s) {
        s.alarm.playing = true;
        s.alarm.active_alarm_id = alarm.id;
        s.alarm.stop_requested = false;
    });

    if (!WavPlayer::getInstance().playAsync(alarm.tone_file)) {
        ESP_LOGE(TAG, "Failed to start WAV playback for alarm");
        m_playing_alarm = false;
        EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
            s.alarm.playing = false;
            s.alarm.active_alarm_id = 0;
        });
    }
}

void AlarmService::stopActiveAlarm() {
    if (m_playing_alarm) {
        WavPlayer::getInstance().stop();
        m_playing_alarm = false;
        EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
            s.alarm.playing = false;
            s.alarm.stop_requested = true;
            s.alarm.active_alarm_id = 0;
        });
    }
}

void AlarmService::onStateChanged(ComponentMask changed, const SystemState& snap) {
    if (changed & COMP::ALARM) {
        if (snap.alarm.stop_requested && m_playing_alarm) {
            ESP_LOGI(TAG, "onStateChanged: Stop requested. Halting alarm playback.");
            stopActiveAlarm();
        }
    }
    if (changed & COMP::AUDIO) {
        // If we were playing an alarm and the WAV player finished (wav_playing transitioned to false)
        if (m_playing_alarm && !snap.audio.wav_playing) {
            ESP_LOGI(TAG, "onStateChanged: WAV playback finished. Clearing alarm state.");
            m_playing_alarm = false;
            EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                s.alarm.playing = false;
                s.alarm.active_alarm_id = 0;
            });
        }
    }
}

void AlarmService::run() {
    ESP_LOGI(TAG, "AlarmService running...");
    static int last_triggered_min = -1;

    while (m_running) {
        uint32_t changed_bits = 0;
        // Check for updates every 5 seconds, or respond to DB changes immediately
        BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &changed_bits, pdMS_TO_TICKS(5000));
        if (!m_running) break;

        if (notified == pdTRUE && changed_bits > 0) {
            m_last_changed = changed_bits;
            SystemState snap = EmbeddedSysDb::getInstance().snapshot();
            onStateChanged(m_last_changed, snap);
        }

        // Get system time
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);

        // Only scan alarms if time is synchronized (RTC year > 2020)
        if (timeinfo.tm_year > 120) {
            if (last_triggered_min != timeinfo.tm_min) {
                std::lock_guard<std::mutex> lock(m_alarms_mutex);
                for (const auto& alarm : m_alarms) {
                    if (alarm.enabled && !m_playing_alarm) {
                        if (alarm.hour == timeinfo.tm_hour && alarm.minute == timeinfo.tm_min) {
                            ESP_LOGI(TAG, "Alarm conditions met. Triggering alarm %d...", alarm.id);
                            triggerAlarm(alarm);
                            last_triggered_min = timeinfo.tm_min;
                            break;
                        }
                    }
                }
            }
        }
    }
}

} // namespace Services
