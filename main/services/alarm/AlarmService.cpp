#include "AlarmService.h"
#include "sd_storage/Fs.h"
#include "services/BufferManager.h"
#include "app/audio/SpeakerPlayback.h"
#include "app/audio/AudioOrchestrator.h"
#include "app/media_player/NexusPlayer.h"
#include "media_player/CatalogDB.h"
#include "app/wake_word/WakeWordEngine.h"
#include "app/audio/recording/AudioRecorder.h"
#include "audio_core/AlertPlayer.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <ctime>
#include <cstring>
#include <memory>
#include <sys/time.h>
#include <ArduinoJson.h>

namespace Services {

namespace {

constexpr const char* ALARMS_FILE = "/sdcard/alarms.json";
// How often a ringing alarm checks song progress, the ring limit and snooze.
constexpr uint32_t TICK_MS = 250;

uint64_t nowMs() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

// Old alarms.json entries hold WAV paths; those (and empty) mean the built-in tone.
std::string toneFromFile(const char* tone_file) {
    std::string t = tone_file ? tone_file : "";
    if (t.empty() || t[0] == '/' || (t.size() > 4 && t.compare(t.size() - 4, 4, ".wav") == 0)) return "";
    return t;
}

const char* actionName(AlarmRing::Action a) {
    switch (a) {
    case AlarmRing::Action::PlaySong: return "song";
    case AlarmRing::Action::PlayBuiltin: return "builtin";
    case AlarmRing::Action::Silence: return "silence";
    case AlarmRing::Action::Finish: return "finish";
    default: return "none";
    }
}

} // namespace

AlarmService& AlarmService::getInstance() {
    static AlarmService instance;
    return instance;
}

AlarmService::AlarmService()
    : ReactorTask({
          "alarm_svc",
          ThreadConfig::StackSize::STACK_LARGE,
          ThreadConfig::Priority::LOW,
          ThreadConfig::CORE_NETWORK,
          COMP::ALARM
       })
{}

bool AlarmService::begin() {
    loadAlarms();
    // A music command from the dashboard or a key while the alarm owns the
    // player: the alarm ends first, without bringing back the old music.
    NexusPlayer::getInstance().setAlarmYieldHandler([this]() {
        stopActiveAlarm(false, 3000);
    });
    return true;
}

void AlarmService::loadAlarms() {
    std::lock_guard<std::mutex> lock(m_alarms_mutex);
    m_alarms.clear();

    if (!sd_storage::Fs::isFile(ALARMS_FILE)) {
        ESP_LOGI(TAG, "No alarms.json on the SD card");
        return;
    }

    std::string content = sd_storage::Fs::readText(ALARMS_FILE);
    if (content.empty()) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, content);
    if (err) {
        ESP_LOGE(TAG, "Failed to parse alarms.json: %s", err.c_str());
        return;
    }

    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
        Alarm alarm = {};
        alarm.id = obj["id"];
        alarm.hour = obj["hour"];
        alarm.minute = obj["minute"];
        strncpy(alarm.tone_file, obj["tone_file"] | "", sizeof(alarm.tone_file) - 1);
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
    sd_storage::Fs::writeAtomic(ALARMS_FILE, content.c_str());
}

void AlarmService::addOrUpdateAlarm(const Alarm& alarm) {
    {
        std::lock_guard<std::mutex> lock(m_alarms_mutex);
        bool found = false;
        for (auto& item : m_alarms) {
            if (item.id == alarm.id) {
                item = alarm;
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

// ── Requests from other tasks ────────────────────────────────────────────────

bool AlarmService::post(Command cmd, uint32_t timeout_ms) {
    // Waiting on ourselves would deadlock; this task handles it inline.
    if (m_task_handle && xTaskGetCurrentTaskHandle() == m_task_handle) {
        handle(cmd);
        return true;
    }
    SemaphoreHandle_t done = nullptr;
    if (timeout_ms > 0 && m_task_handle) {
        done = xSemaphoreCreateBinary();
        cmd.done = done;
    }
    {
        std::lock_guard<std::mutex> lock(m_cmd_mutex);
        m_cmds.push_back(std::move(cmd));
    }
    if (m_task_handle) xTaskNotify(m_task_handle, 0, eNoAction);
    if (!done) return true;
    bool ok = xSemaphoreTake(done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    if (ok) {
        vSemaphoreDelete(done);
    } else {
        // The task still owns the command and will give the semaphore later;
        // leak it rather than free it under the task (rare: task stalled).
        ESP_LOGW(TAG, "Alarm request timed out");
    }
    return ok;
}

bool AlarmService::ring(const RingOptions& opts, uint32_t timeout_ms) {
    Command c{CmdType::Ring};
    c.ring = opts;
    return post(std::move(c), timeout_ms);
}

bool AlarmService::stopActiveAlarm(bool restore_media, uint32_t timeout_ms) {
    Command c{CmdType::Stop};
    c.restore = restore_media;
    return post(std::move(c), timeout_ms);
}

bool AlarmService::snooze(uint32_t timeout_ms) {
    return post(Command{CmdType::Snooze}, timeout_ms);
}

void AlarmService::onTrackFinished(const char*) {
    post(Command{CmdType::SongEnded}, 0);
}

void AlarmService::onPlaybackError(const char* songId, int errorCode) {
    ESP_LOGW(TAG, "Alarm song %s failed (%d)", songId ? songId : "?", errorCode);
    post(Command{CmdType::SongFailed}, 0);
}

void AlarmService::onStateChanged(ComponentMask changed, const SystemState& snap) {
    if (!(changed & COMP::ALARM)) return;
    const bool stop = snap.alarm.stop_requested;
    const bool snz = snap.alarm.snooze_requested;
    if (!stop && !snz) return;
    EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
        s.alarm.stop_requested = false;
        s.alarm.snooze_requested = false;
    });
    Command c{stop ? CmdType::Stop : CmdType::Snooze};
    handle(c);
}

AlarmService::Status AlarmService::status() {
    std::lock_guard<std::mutex> lock(m_status_mutex);
    Status st = m_status;
    const uint64_t now_ms = nowMs();
    if (st.state == AlarmRing::State::Ringing) st.ringing_ms = (uint32_t)(now_ms - m_status_ring_start);
    if (st.state == AlarmRing::State::Snoozed) {
        st.snooze_left_ms = m_status_snooze_end > now_ms ? (uint32_t)(m_status_snooze_end - now_ms) : 0;
    }
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    st.time_synced = t.tm_year > 120;
    return st;
}

// ── Ringing (this task only) ─────────────────────────────────────────────────

void AlarmService::handle(Command& cmd) {
    const uint64_t now = nowMs();
    AlarmRing::Action action = AlarmRing::Action::None;
    switch (cmd.type) {
    case CmdType::Ring: {
        if (m_ring.state() == AlarmRing::State::Ringing) {
            ESP_LOGI(TAG, "Alarm %d ignored: another alarm is ringing", cmd.ring.alarm_id);
            break;
        }
        AlarmRing::Config cfg;
        if (cmd.ring.ring_limit_ms) cfg.ring_limit_ms = cmd.ring.ring_limit_ms;
        if (cmd.ring.snooze_ms) cfg.snooze_ms = cmd.ring.snooze_ms;
        m_ring.setConfig(cfg);
        m_tone = cmd.ring.tone;
        m_tone_title.clear();
        m_fallback_reason.clear();
        bool has_song = false;
        if (!m_tone.empty()) {
            auto rec = std::make_unique<TrackRecord>();
            if (!CatalogDB::getInstance().get(m_tone.c_str(), *rec)) {
                m_fallback_reason = "tone not in the library";
            } else if (!NexusPlayer::getInstance().getStorageManager().fileExists(m_tone.c_str())) {
                m_fallback_reason = "tone file missing";
            } else {
                m_tone_title = rec->title;
                has_song = true;
            }
        }
        ESP_LOGI(TAG, "Alarm %d firing (tone: %s%s%s)", cmd.ring.alarm_id,
                 has_song ? m_tone_title.c_str() : "built-in",
                 m_fallback_reason.empty() ? "" : ", ", m_fallback_reason.c_str());
        action = m_ring.fire(cmd.ring.alarm_id, has_song, now);
        break;
    }
    case CmdType::Stop:
        action = m_ring.stop(now);
        if (action == AlarmRing::Action::Finish && !cmd.restore) {
            ESP_LOGI(TAG, "Alarm stopped by a music command");
            silence();
            giveBack();
            NexusPlayer::getInstance().endAlarm(false);
            action = AlarmRing::Action::None;
        }
        break;
    case CmdType::Snooze:
        action = m_ring.snooze(now);
        break;
    case CmdType::SongEnded:
        action = m_ring.songEnded(now);
        break;
    case CmdType::SongFailed:
        if (m_ring.source() == AlarmRing::Source::Song) m_fallback_reason = "song playback failed";
        action = m_ring.songFailed(now);
        break;
    }
    apply(action);
    publish();
    if (cmd.done) xSemaphoreGive(cmd.done);
}

void AlarmService::apply(AlarmRing::Action action) {
    if (action == AlarmRing::Action::None) return;
    ESP_LOGI(TAG, "action: %s", actionName(action));
    switch (action) {
    case AlarmRing::Action::PlaySong:
        if (!m_taken_over) takeOver();
        startSong();
        break;
    case AlarmRing::Action::PlayBuiltin:
        if (!m_taken_over) takeOver();
        startBuiltin(m_fallback_reason.empty() ? "no tone set" : m_fallback_reason.c_str());
        break;
    case AlarmRing::Action::Silence:
        silence();
        break;
    case AlarmRing::Action::Finish:
        silence();
        giveBack();
        NexusPlayer::getInstance().endAlarm(true);
        if (m_ring.lastEnd() == AlarmRing::EndReason::TimedOut) {
            ESP_LOGW(TAG, "Alarm %d rang for %u min without an answer; stopped", m_ring.alarmId(),
                     (unsigned)(m_ring.config().ring_limit_ms / 60000));
        }
        break;
    default:
        break;
    }
}

void AlarmService::takeOver() {
    const int64_t t0 = esp_timer_get_time();
    m_taken_over = true;
    AudioOrchestrator::getInstance().notifyAlarmStarted();
    AlertPlayer::getInstance().setAlarmActive(true);
    WakeWordEngine::getInstance().setWakeWordSuppressed(true);

    // End any assistant session; the alarm is the only sound.
    auto& db = EmbeddedSysDb::getInstance();
    SystemState snap = db.snapshot();
    if (snap.assistant.session_state != AssistantState::Idle) {
        ESP_LOGI(TAG, "Ending the assistant session for the alarm");
        db.mutate([](SystemState& s) {
            s.assistant.session_state = AssistantState::Idle;
            s.assistant.visual_state = s.system.wifi_connected ? AssistantVisualState::Idle
                                                               : AssistantVisualState::Offline;
        });
        BufferManager::getInstance().flush(Buffers::VOICE_RX_BUF);
    }

    m_saved_volume = snap.audio.speaker_volume;
    m_raised_volume = -1;
    if (m_saved_volume < MIN_VOLUME) {
        m_raised_volume = MIN_VOLUME;
        db.mutate([](SystemState& s) { s.audio.speaker_volume = MIN_VOLUME; });
    }

    NexusPlayer::getInstance().beginAlarm(this);
    ESP_LOGI(TAG, "Device taken over in %lld us (volume %d -> %d)", (long long)(esp_timer_get_time() - t0),
             m_saved_volume, m_raised_volume < 0 ? m_saved_volume : m_raised_volume);
}

void AlarmService::giveBack() {
    if (!m_taken_over) return;
    m_taken_over = false;
    if (m_raised_volume >= 0) {
        const int saved = m_saved_volume, raised = m_raised_volume;
        // Put the volume back unless someone changed it while ringing.
        EmbeddedSysDb::getInstance().mutate([saved, raised](SystemState& s) {
            if (s.audio.speaker_volume == raised) s.audio.speaker_volume = saved;
        });
    }
    m_saved_volume = m_raised_volume = -1;
    // The recorder suppresses the wake word too; leave it off while it records.
    if (!AudioRecorder::getInstance().isRecording()) {
        WakeWordEngine::getInstance().setWakeWordSuppressed(false);
    }
    AlertPlayer::getInstance().setAlarmActive(false);
    AudioOrchestrator::getInstance().notifyAlarmEnded();
}

void AlarmService::startSong() {
    AlertPlayer::getInstance().stopAlarmTone();
    const int64_t t0 = esp_timer_get_time();
    if (!NexusPlayer::getInstance().playAlarm(m_tone.c_str())) {
        m_fallback_reason = "song did not start";
        apply(m_ring.songFailed(nowMs()));
        return;
    }
    AudioOrchestrator::getInstance().fadeInMedia(FADE_FROM, FADE_MS);
    ESP_LOGI(TAG, "Alarm song %s started in %lld us", m_tone.c_str(), (long long)(esp_timer_get_time() - t0));
}

void AlarmService::startBuiltin(const char* reason) {
    ESP_LOGW(TAG, "Alarm rings the built-in tone (%s)", reason);
    NexusPlayer::getInstance().stopAlarmSong();
    AlertPlayer::getInstance().startAlarmTone();
}

void AlarmService::silence() {
    AlertPlayer::getInstance().stopAlarmTone();
    NexusPlayer::getInstance().stopAlarmSong();
}

void AlarmService::publish() {
    const uint64_t now = nowMs();
    Status st;
    st.state = m_ring.state();
    st.source = m_ring.source();
    st.last_end = m_ring.lastEnd();
    st.alarm_id = m_ring.alarmId();
    st.tone = m_tone;
    st.tone_title = m_tone_title;
    st.fallback_reason = m_ring.source() == AlarmRing::Source::Builtin ? m_fallback_reason : "";
    st.ringing_ms = m_ring.ringingMs(now);
    st.snooze_left_ms = m_ring.snoozeLeftMs(now);
    st.snoozes = m_ring.snoozeCount();
    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        m_status = st;
        m_status_ring_start = now - st.ringing_ms;
        m_status_snooze_end = now + st.snooze_left_ms;
    }

    const AlarmRingState state = st.state == AlarmRing::State::Ringing ? AlarmRingState::RINGING
                               : st.state == AlarmRing::State::Snoozed ? AlarmRingState::SNOOZED
                                                                       : AlarmRingState::IDLE;
    const uint32_t snooze_until = st.snooze_left_ms ? (uint32_t)time(nullptr) + st.snooze_left_ms / 1000 : 0;
    const bool builtin = st.source == AlarmRing::Source::Builtin;
    const int id = state == AlarmRingState::IDLE ? 0 : st.alarm_id;
    EmbeddedSysDb::getInstance().mutate([=](SystemState& s) {
        s.alarm.state = state;
        s.alarm.playing = state == AlarmRingState::RINGING;
        s.alarm.active_alarm_id = id;
        s.alarm.using_builtin = builtin;
        s.alarm.snooze_until = snooze_until;
    });
}

// ── Scheduler ────────────────────────────────────────────────────────────────

uint32_t AlarmService::msToNextMinute() const {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const uint32_t into = (uint32_t)(tv.tv_sec % 60) * 1000 + (uint32_t)(tv.tv_usec / 1000);
    return 60000 - into;
}

void AlarmService::checkSchedule() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year <= 120) {
        if (!m_warned_unsynced) {
            ESP_LOGW(TAG, "Clock not set; alarms wait for time sync");
            m_warned_unsynced = true;
        }
        return;
    }
    // Each local minute is checked once; a minute passed while off is skipped.
    const int64_t key = ((int64_t)(t.tm_year * 366 + t.tm_yday) * 24 + t.tm_hour) * 60 + t.tm_min;
    if (key == m_last_minute_key) return;
    m_last_minute_key = key;

    RingOptions due;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_alarms_mutex);
        for (const auto& a : m_alarms) {
            if (a.enabled && a.hour == t.tm_hour && a.minute == t.tm_min) {
                due.alarm_id = a.id;
                due.tone = toneFromFile(a.tone_file);
                found = true;
                break;
            }
        }
    }
    if (found) {
        Command c{CmdType::Ring};
        c.ring = due;
        handle(c);
    }
}

void AlarmService::run() {
    ESP_LOGI(TAG, "Alarm scheduler running");
    publish();

    while (m_running) {
        const bool active = m_ring.state() != AlarmRing::State::Idle;
        // Idle: wake just after the next minute starts. Ringing: tick.
        const uint32_t wait_ms = active ? TICK_MS : msToNextMinute() + 20;
        uint32_t bits = 0;
        BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &bits, pdMS_TO_TICKS(wait_ms));
        if (!m_running) break;

        if (notified == pdTRUE && bits != 0) {
            m_last_changed = bits;
            onStateChanged(bits, EmbeddedSysDb::getInstance().snapshot());
        }

        for (;;) {
            Command cmd;
            {
                std::lock_guard<std::mutex> lock(m_cmd_mutex);
                if (m_cmds.empty()) break;
                cmd = std::move(m_cmds.front());
                m_cmds.pop_front();
            }
            handle(cmd);
        }

        if (m_ring.state() == AlarmRing::State::Ringing && m_ring.source() == AlarmRing::Source::Song &&
            NexusPlayer::getInstance().getPositionMs() > 0) {
            m_ring.songProgress(nowMs());
        }
        const AlarmRing::State before = m_ring.state();
        AlarmRing::Action action = m_ring.tick(nowMs());
        if (action == AlarmRing::Action::PlayBuiltin && m_fallback_reason.empty()) {
            m_fallback_reason = "song silent after start";
        }
        apply(action);
        if (action != AlarmRing::Action::None || m_ring.state() != before) publish();

        checkSchedule();
    }
}

} // namespace Services
