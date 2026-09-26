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
#include "services/time/TimeSyncHelper.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
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
// Longest idle sleep, so clock changes are noticed.
constexpr uint32_t MAX_IDLE_MS = 60000;
// A clock jump larger than this skips what it jumped over.
constexpr int64_t MAX_GAP_S = 90;

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
    // A music command from the dashboard or a key while the alarm owns the
    // player: the alarm ends first, without bringing back the old music.
    NexusPlayer::getInstance().setAlarmYieldHandler([this]() {
        stopActiveAlarm(false, 3000);
    });
    return true;
}

// ── Alarms ───────────────────────────────────────────────────────────────────

AlarmWhen AlarmService::whenOf(const AlarmDoc& doc) {
    AlarmWhen w;
    w.hour = doc.hour;
    w.minute = doc.minute;
    w.days = doc.days;
    w.at = doc.at;
    return w;
}

int64_t AlarmService::nextFireOf(const AlarmDoc& doc, int64_t now) {
    if (!doc.enabled) return 0;
    const AlarmWhen w = whenOf(doc);
    // A one-shot hour:minute fires once, at its first occurrence after creation.
    int64_t after = now;
    if (w.oneShot() && !w.at) {
        if (doc.last_fired) return 0;
        after = std::max<int64_t>(now, doc.created);
    }
    return nextFire(w, after);
}

std::vector<std::pair<int, AlarmDoc>> AlarmService::alarms() {
    return listAlarms();
}

int AlarmService::saveAlarm(int id, AlarmDoc doc) {
    if (id <= 0) id = nextAlarmId();
    AlarmDoc old;
    const bool exists = loadAlarm(id, old);
    doc.created = exists && old.created ? old.created : (uint32_t)time(nullptr);
    // A changed alarm starts over.
    doc.last_fired = 0;
    doc.snooze_until = 0;
    if (!Services::saveAlarm(id, doc)) {
        ESP_LOGE(TAG, "Could not save alarm %d", id);
        return 0;
    }
    ESP_LOGI(TAG, "Alarm %d saved: %02u:%02u days 0x%02x at %lu%s", id, doc.hour, doc.minute, doc.days,
             (unsigned long)doc.at, doc.enabled ? "" : " (disabled)");
    if (m_task_handle) xTaskNotify(m_task_handle, 0, eNoAction);   // reschedule
    return id;
}

bool AlarmService::deleteAlarm(int id) {
    AlarmDoc doc;
    if (!loadAlarm(id, doc)) return false;
    const Status st = status();
    if (st.state != AlarmRing::State::Idle && st.alarm_id == id) stopActiveAlarm(true, 3000);
    const bool ok = removeAlarm(id);
    if (ok) ESP_LOGI(TAG, "Alarm %d deleted", id);
    if (m_task_handle) xTaskNotify(m_task_handle, 0, eNoAction);
    return ok;
}

// alarms.json -> the alarms collection, once. Its alarms rang every day.
void AlarmService::migrateAlarmsFile() {
    if (!sd_storage::Fs::isFile(ALARMS_FILE)) return;
    if (!systemDb().db().isOpen()) return;
    JsonDocument doc;
    const std::string content = sd_storage::Fs::readText(ALARMS_FILE);
    if (deserializeJson(doc, content) || !doc.is<JsonArray>()) {
        ESP_LOGW(TAG, "alarms.json is not a JSON array; not importing it");
    } else {
        int imported = 0;
        for (JsonObject obj : doc.as<JsonArray>()) {
            const int id = obj["id"] | 0;
            const int hour = obj["hour"] | -1;
            const int minute = obj["minute"] | -1;
            AlarmDoc existing;
            if (id <= 0 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || loadAlarm(id, existing)) continue;
            AlarmDoc a;
            a.hour = (uint8_t)hour;
            a.minute = (uint8_t)minute;
            a.days = AlarmWhen::EVERY_DAY;
            a.enabled = obj["enabled"] | true;
            a.tone = toneFromFile(obj["tone_file"] | "");
            a.created = (uint32_t)time(nullptr);
            if (!Services::saveAlarm(id, a)) {
                ESP_LOGE(TAG, "Could not import alarms.json; left in place");
                return;
            }
            ++imported;
        }
        ESP_LOGI(TAG, "Imported %d alarms from alarms.json", imported);
    }
    const std::string bak = std::string(ALARMS_FILE) + ".bak";
    sd_storage::Fs::remove(bak.c_str());
    sd_storage::Fs::rename(ALARMS_FILE, bak.c_str());
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
        m_min_volume = cmd.ring.volume > 0 ? std::min(cmd.ring.volume, 100) : MIN_VOLUME;
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
    if (m_saved_volume < m_min_volume) {
        const int floor = m_raised_volume = m_min_volume;
        db.mutate([floor](SystemState& s) { s.audio.speaker_volume = floor; });
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
    persistSnooze(state == AlarmRingState::SNOOZED ? id : 0, snooze_until);
    EmbeddedSysDb::getInstance().mutate([=](SystemState& s) {
        s.alarm.state = state;
        s.alarm.playing = state == AlarmRingState::RINGING;
        s.alarm.active_alarm_id = id;
        s.alarm.using_builtin = builtin;
        s.alarm.snooze_until = snooze_until;
    });
}

// ── Scheduler ────────────────────────────────────────────────────────────────

void AlarmService::persistSnooze(int id, uint32_t until) {
    if (id == m_snooze_saved_id && (id == 0 || until == m_snooze_saved_until)) return;
    AlarmDoc doc;
    if (m_snooze_saved_id > 0 && m_snooze_saved_id != id) {
        doc.snooze_until = 0;
        mergeAlarm(m_snooze_saved_id, doc, AlarmDoc::F_SNOOZE_UNTIL);
    }
    if (id > 0 && loadAlarm(id, doc)) {
        doc.snooze_until = until;
        mergeAlarm(id, doc, AlarmDoc::F_SNOOZE_UNTIL);
    }
    m_snooze_saved_id = id;
    m_snooze_saved_until = until;
}

uint32_t AlarmService::checkSchedule() {
    if (!TimeSyncHelper::clockValid()) {
        if (!m_warned_unsynced) {
            ESP_LOGW(TAG, "Clock not set; alarms wait for time sync");
            m_warned_unsynced = true;
        }
        m_checked_until = 0;
        return MAX_IDLE_MS;
    }
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const int64_t now = tv.tv_sec;
    // First valid time, or the clock jumped: what lies before now is skipped.
    if (m_checked_until == 0 || now < m_checked_until || now - m_checked_until > MAX_GAP_S) {
        if (m_checked_until != 0) ESP_LOGW(TAG, "Clock jumped by %lld s; skipping", (long long)(now - m_checked_until));
        m_checked_until = now;
        m_warned_unsynced = false;
    }

    const bool ringing = m_ring.state() == AlarmRing::State::Ringing;
    const int snoozed_id = m_ring.state() == AlarmRing::State::Snoozed ? m_ring.alarmId() : 0;
    int64_t next = 0;
    auto consider = [&](int64_t t) { if (t > now && (next == 0 || t < next)) next = t; };

    int due_id = 0;
    AlarmDoc due;
    for (auto& [id, a] : listAlarms()) {
        // A snooze saved before a reboot (the running one lives in m_ring).
        if (a.snooze_until && id != snoozed_id) {
            if (a.snooze_until > now) {
                consider(a.snooze_until);
            } else if (!due_id && !ringing && now - a.snooze_until <= SNOOZE_GRACE_S) {
                ESP_LOGI(TAG, "Alarm %d: snooze from before the restart ended", id);
                due_id = id;
                due = a;
            } else {
                AlarmDoc clear;
                mergeAlarm(id, clear, AlarmDoc::F_SNOOZE_UNTIL);
            }
            if (a.snooze_until > now || due_id == id) continue;
        }
        if (!a.enabled) continue;
        const int64_t f = nextFireOf(a, std::max<int64_t>(m_checked_until, a.last_fired));
        if (f == 0) {
            if (a.kind == 1) removeAlarm(id);   // a timer that ran out while off
            continue;
        }
        if (f > now) {
            consider(f);
            continue;
        }
        // Due now.
        const bool timer = a.kind == 1;
        if (whenOf(a).oneShot() || timer) {
            if (timer) {
                removeAlarm(id);
            } else {
                AlarmDoc upd;
                upd.enabled = false;
                upd.last_fired = (uint32_t)now;
                mergeAlarm(id, upd, AlarmDoc::F_ENABLED | AlarmDoc::F_LAST_FIRED);
            }
        } else {
            AlarmDoc upd;
            upd.last_fired = (uint32_t)now;
            mergeAlarm(id, upd, AlarmDoc::F_LAST_FIRED);
            consider(nextFireOf(a, now));
        }
        if (due_id || ringing) {
            ESP_LOGW(TAG, "Alarm %d due while alarm %d rings; skipped", id, due_id ? due_id : m_ring.alarmId());
            continue;
        }
        due_id = id;
        due = a;
    }
    m_checked_until = now;

    if (due_id) {
        Command c{CmdType::Ring};
        c.ring.alarm_id = due_id;
        c.ring.tone = due.tone;
        c.ring.snooze_ms = (uint32_t)(due.snooze_min ? due.snooze_min : 9) * 60000;
        c.ring.volume = due.volume;
        handle(c);
    }

    if (next == 0) return MAX_IDLE_MS;
    // Just after the second it is due.
    const int64_t ms = (next - now) * 1000 - tv.tv_usec / 1000 + 20;
    return (uint32_t)std::min<int64_t>(std::max<int64_t>(ms, 20), MAX_IDLE_MS);
}

void AlarmService::run() {
    ESP_LOGI(TAG, "Alarm scheduler running");
    migrateAlarmsFile();
    publish();
    uint32_t schedule_ms = 0;

    while (m_running) {
        const bool active = m_ring.state() != AlarmRing::State::Idle;
        // Idle: sleep until the next alarm is due. Ringing: tick.
        const uint32_t wait_ms = active ? std::min(TICK_MS, schedule_ms) : schedule_ms;
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

        schedule_ms = checkSchedule();
    }
}

} // namespace Services
