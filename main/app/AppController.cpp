#include "AppController.h"
#include "app/audio/AudioService.h"
#include "gemini_live/GeminiProtocol.h"
#include "gemini_live/gemini_skills_generated.h"
#include "app/led/LedService.h"
#include "gemini_live/AssistantService.h"
#include "gemini_live/MediaCommandHandler.h"
#include "gemini_live/DeviceCommandHandler.h"
#include "services/time/TimeSyncHelper.h"
#include "services/storage/StorageService.h"
#include "services/alarm/AlarmService.h"
#include "app/mqtt/MqttService.h"

#include "common/AppLogger.h"
#include "common/AsyncNetLogger.h"
#include "common/LogRouter.h"
#include "common/thread_config.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "hal/Board.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include <string>
#include <cstring>
#include <ArduinoJson.h>

AppController::AppController()
    : ReactorTask({
          "app_ctrl",
          ThreadConfig::StackSize::STACK_NORMAL,
          ThreadConfig::Priority::LOW,
          ThreadConfig::CORE_NETWORK,
          COMP::SYSTEM | COMP::AUDIO | COMP::BLUETOOTH
      })
{}

AppController &AppController::getInstance() {
    static AppController instance;
    return instance;
}

bool AppController::begin() {
    // Register platform delegate for local device commands
    DeviceCommandHandler::setDelegate(this);

    // Register tool-call handler callback with GeminiProtocol
    GeminiProtocol::getInstance().setToolCallHandler(handleGeminiToolCall, this);

    m_wifi_connected = EmbeddedSysDb::getInstance().snapshot().system.wifi_connected;

    LOGI_SYSTEM("AppController initialized.");
    return true;
}

void AppController::onStateChanged(ComponentMask changed, const SystemState& snap) {
    // NOTE: per-field BIT_* values are only unique *within* a component. diffState()
    // OR's them together as (COMP::X | BIT_X::FIELD), so every per-field test must
    // also gate on its COMP:: bit.
    if ((changed & COMP::AUDIO) && (changed & BIT_AUDIO::SPEAKER_VOLUME)) {
        // Speaker volume changed
    }

    if ((changed & COMP::SYSTEM) && (changed & BIT_SYSTEM::WIFI_CONNECTED)) {
        bool wifi_ok = snap.system.wifi_connected;

        if (wifi_ok && !m_time_synced) {
            m_time_synced = true;
            LOGI_SYSTEM("Wi-Fi connected. Spawning background NTP synchronization task in PSRAM...");
            BaseType_t ret = xTaskCreateWithCaps([](void* arg) {
                Services::TimeSyncHelper::synchronizeTimeAndCleanup();
                vTaskDelete(NULL);
            }, "ntp_sync", 4096, NULL, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (ret != pdPASS) {
                xTaskCreate([](void* arg) {
                    Services::TimeSyncHelper::synchronizeTimeAndCleanup();
                    vTaskDelete(NULL);
                }, "ntp_sync", 3072, NULL, 4, NULL);
            }
        }
        
        // if (wifi_ok && !m_wifi_connected) {
        //     m_wifi_connected = true;
        //     LOGI_SYSTEM("Network connected. Initializing net logging...");
        //     // AsyncNetLogger::getInstance().init(snap.system.server_ip, 5006);
        //     // AsyncNetLogger::getInstance().startWorker();
        //     LogRouter::getInstance().setNetworkStreamingState(
        //         LogRouter::State::ROUTE_CONSOLE_ONLY);
        // } else if (!wifi_ok && m_wifi_connected) {
        //     m_wifi_connected = false;
        //     LOGW_SYSTEM("Network disconnected. Stopping net logging...");
        //     LogRouter::getInstance().setNetworkStreamingState(
        //         LogRouter::State::ROUTE_CONSOLE_ONLY);
        //     AsyncNetLogger::getInstance().stopWorker();
        // }
    }
}

void AppController::handleGeminiToolCall(const GeminiSkills::DecodedSkillCall& skill_call, void* ctx) {
    auto self = static_cast<AppController*>(ctx);
    if (self) {
        self->executeToolCall(skill_call);
    }
}

void AppController::executeToolCall(const GeminiSkills::DecodedSkillCall& skill_call) {
    JsonDocument response_doc;
    bool is_media_command = false;

    // Try device/local commands first; if not handled, fall back to media command handler
    if (!DeviceCommandHandler::handle(skill_call, response_doc)) {
        if (MediaCommandHandler::handle(skill_call, response_doc)) {
            is_media_command = true;
        } else {
            response_doc["status"] = "error";
            response_doc["message"] = "Unsupported tool skill";
        }
    }

    std::string feedback_string;
    serializeJson(response_doc, feedback_string);

    LOGI_SYSTEM("Uplinking tool response: %s", feedback_string.c_str());
    GeminiProtocol::getInstance().transmitToolResponse(skill_call.call_id, feedback_string.c_str());

    // If it was a media command, set the media_pending_idle flag to trigger immediate session termination once speaking finishes
    if (is_media_command) {
        LOGI_SYSTEM("Media command handled. Setting pending idle flag to bypass VAD delay after speech confirmation.");
        auto& sysdb = EmbeddedSysDb::getInstance();
        sysdb.mutate([](SystemState& s) {
            s.assistant.media_pending_idle = true;
        });
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IDeviceCommandDelegate Implementation
// ─────────────────────────────────────────────────────────────────────────────

bool AppController::writeFile(const char* path, const char* content) {
    return Services::StorageService::getInstance().writeFile(path, content);
}

std::string AppController::readFile(const char* path) {
    return Services::StorageService::getInstance().readFile(path);
}

bool AppController::fileExists(const char* path) {
    return Services::StorageService::getInstance().fileExists(path);
}

bool AppController::appendFile(const char* path, const char* content) {
    return Services::StorageService::getInstance().appendFile(path, content);
}

bool AppController::publishMqtt(const char* topic, const char* message) {
#if CONFIG_WAVESHARE_MQTT_ENABLE
    return MqttService::getInstance().publish(topic, message);
#else
    return false;
#endif
}

bool AppController::setAlarm(int hour, int minute, const char* tone_file, bool enabled, int& out_alarm_id) {
    auto alarms = Services::AlarmService::getInstance().getAlarms();
    int target_id = -1;
    int max_id = 0;
    for (const auto& a : alarms) {
        if (a.id > max_id) {
            max_id = a.id;
        }
        if (a.hour == hour && a.minute == minute) {
            target_id = a.id;
        }
    }

    if (target_id == -1) {
        target_id = max_id + 1;
    }

    Services::Alarm alarm;
    alarm.id = target_id;
    alarm.hour = hour;
    alarm.minute = minute;
    alarm.enabled = enabled;

    std::string tone = (tone_file && tone_file[0] != '\0') ? tone_file : "/sdcard/alarms/soft_wake_up.wav";
    strncpy(alarm.tone_file, tone.c_str(), sizeof(alarm.tone_file) - 1);
    alarm.tone_file[sizeof(alarm.tone_file) - 1] = '\0';

    ESP_LOGI(TAG, "Tool request: Setting alarm %d for %02d:%02d (%s, %s)...", 
             alarm.id, alarm.hour, alarm.minute, alarm.tone_file, alarm.enabled ? "enabled" : "disabled");
    
    Services::AlarmService::getInstance().addOrUpdateAlarm(alarm);
    out_alarm_id = target_id;
    return true;
}

bool AppController::stopActiveAlarm() {
    ESP_LOGI(TAG, "Tool request: Stopping active alarm tone...");
    Services::AlarmService::getInstance().stopActiveAlarm();
    return true;
}

