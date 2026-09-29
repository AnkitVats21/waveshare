#include "AlarmService.h"
#include "sd_storage/Fs.h"
#include "services/BufferManager.h"
#include "app/audio/SpeakerPlayback.h"
#include "app/audio/AudioOrchestrator.h"
#include "app/media_player/NexusPlayer.h"
#include "media_player/CatalogDB.h"
#include "app/wake_word/WakeWordEngine.h"
#include "gemini_live/GeminiProtocol.h"
#include "gemini_live/AssistantService.h"
#include "app/audio/recording/AudioRecorder.h"
#include "audio_core/AlertPlayer.h"
#include "services/storage/SystemDatabase.h"
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

namespace {

// Alarms and reminders share the "when" fields.
template <typename Doc>
AlarmWhen whenFields(const Doc& doc) {
    AlarmWhen w;
    w.hour = doc.hour;
    w.minute = doc.minute;
    w.days = doc.days;
    w.at = doc.at;
    return w;
}

template <typename Doc>
int64_t nextFireFields(const Doc& doc, int64_t now) {
    if (!doc.enabled) return 0;
    const AlarmWhen w = whenFields(doc);
    // A one-shot hour:minute fires once, at its first occurrence after creation.
    int64_t after = now;
    if (w.oneShot() && !w.at) {
        if (doc.last_fired) return 0;
        after = std::max<int64_t>(now, doc.created);
    }
    return nextFire(w, after);
}

} // namespace

int64_t AlarmService::nextFireOf(const AlarmDoc& doc, int64_t now) {
    return nextFireFields(doc, now);
}

int64_t AlarmService::nextFireOf(const ReminderDoc& doc, int64_t now) {
    return nextFireFields(doc, now);
}

AlarmWhen AlarmService::whenOf(const AlarmDoc& doc) {
    return whenFields(doc);
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

// ── Reminders ────────────────────────────────────────────────────────────────

std::vector<std::pair<int, ReminderDoc>> AlarmService::reminders() {
    return listReminders();
}

int AlarmService::saveReminder(int id, ReminderDoc doc) {
    if (id <= 0) id = nextReminderId();
    ReminderDoc old;
    const bool exists = loadReminder(id, old);
    doc.created = exists && old.created ? old.created : (uint32_t)time(nullptr);
    doc.last_fired = 0;
    doc.pending = false;
    if (!Services::saveReminder(id, doc)) {
        ESP_LOGE(TAG, "Could not save reminder %d", id);
        return 0;
    }
    ESP_LOGI(TAG, "Reminder %d saved: %02u:%02u days 0x%02x at %lu%s", id, doc.hour, doc.minute, doc.days,
             (unsigned long)doc.at, doc.enabled ? "" : " (disabled)");
    if (m_task_handle) xTaskNotify(m_task_handle, 0, eNoAction);
    return id;
}

bool AlarmService::deleteReminder(int id) {
    ReminderDoc doc;
    if (!loadReminder(id, doc) || !removeReminder(id)) return false;
    ESP_LOGI(TAG, "Reminder %d deleted", id);
    if (m_task_handle) xTaskNotify(m_task_handle, 0, eNoAction);
    return true;
}

bool AlarmService::acknowledgeReminder(int id) {
    ReminderDoc doc;
    if (!loadReminder(id, doc)) return false;
    if (doc.pending) {
        doc.pending = false;
        mergeReminder(id, doc, ReminderDoc::F_PENDING);
        ESP_LOGI(TAG, "Reminder %d acknowledged", id);
    }
    return true;
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
        m_tone_path.clear();
        m_builtin_tone.clear();
        m_fallback_reason.clear();
        bool has_song = false;
        const AlarmTone tone = parseTone(m_tone);
        if (tone.kind == AlarmTone::Kind::Builtin) {
            if (AlertPlayer::alarmToneIndex(tone.value.c_str()) >= 0) m_builtin_tone = tone.value;
        } else if (tone.kind == AlarmTone::Kind::File) {
            // Played by path; the song id is only a label for the player.
            m_tone_path = std::string(ALARM_TONE_DIR) + "/" + tone.value;
            if (!isValidToneFileName(tone.value)) {
                m_fallback_reason = "invalid tone file name";
            } else if (!sd_storage::Fs::isFile(m_tone_path.c_str())) {
                m_fallback_reason = "tone file missing";
            } else {
                m_tone_title = tone.value;
                has_song = true;
            }
        } else {
            ndb::music::TrackDoc rec;
            if (!CatalogDB::getInstance().get(m_tone.c_str(), rec)) {
                m_fallback_reason = "tone not in the library";
            } else if (!NexusPlayer::getInstance().getStorageManager().fileExists(m_tone.c_str())) {
                m_fallback_reason = "tone file missing";
            } else {
                m_tone_title = rec.title;
                has_song = true;
            }
        }
        ESP_LOGI(TAG, "Alarm %d firing (tone: %s%s%s)", cmd.ring.alarm_id,
                 has_song ? m_tone_title.c_str() : (m_builtin_tone.empty() ? "built-in" : m_builtin_tone.c_str()),
                 m_fallback_reason.empty() ? "" : ", ", m_fallback_reason.c_str());
        action = m_ring.fire(cmd.ring.alarm_id, has_song, now);
        break;
    }
    case CmdType::Stop:
        if (m_ring.state() == AlarmRing::State::Idle && m_waiting_snooze_id) {
            ESP_LOGI(TAG, "Alarm %d: snooze from before the restart cancelled", m_waiting_snooze_id);
            AlarmDoc clear;
            mergeAlarm(m_waiting_snooze_id, clear, AlarmDoc::F_SNOOZE_UNTIL);
            m_waiting_snooze_id = 0;
            m_waiting_snooze_until = 0;
            break;
        }
        action = m_ring.stop(now);
        if (action == AlarmRing::Action::Finish && cmd.restore) {
            // Stopped by the user (not timed out, not replaced by music).
            AlarmDoc doc;
            if (loadAlarm(m_ring.alarmId(), doc) && doc.briefing && doc.kind == 0) {
                ESP_LOGI(TAG, "Alarm %d stopped; briefing next", m_ring.alarmId());
                m_briefing_due = true;
            }
        }
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
        startBuiltin(m_fallback_reason.empty() ? (m_builtin_tone.empty() ? "default" : "chosen")
                                               : m_fallback_reason.c_str());
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
    const bool started = m_tone_path.empty()
        ? NexusPlayer::getInstance().playAlarm(m_tone.c_str())
        : NexusPlayer::getInstance().playAlarm("alarm_tone", m_tone_path.c_str());
    if (!started) {
        m_fallback_reason = "song did not start";
        apply(m_ring.songFailed(nowMs()));
        return;
    }
    AudioOrchestrator::getInstance().fadeInMedia(FADE_FROM, FADE_MS);
    ESP_LOGI(TAG, "Alarm song %s started in %lld us", m_tone.c_str(), (long long)(esp_timer_get_time() - t0));
}

void AlarmService::startBuiltin(const char* reason) {
    ESP_LOGI(TAG, "Alarm rings the built-in %s tone (%s)", m_builtin_tone.empty() ? "classic" : m_builtin_tone.c_str(),
             reason);
    NexusPlayer::getInstance().stopAlarmSong();
    AlertPlayer::getInstance().startAlarmTone(m_builtin_tone.c_str());
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

    const uint32_t wall = (uint32_t)time(nullptr);
    persistSnooze(st.state == AlarmRing::State::Snoozed ? st.alarm_id : 0,
                  st.snooze_left_ms ? wall + st.snooze_left_ms / 1000 : 0);
    // Reported like a running snooze.
    if (st.state == AlarmRing::State::Idle && m_waiting_snooze_id) {
        st.state = AlarmRing::State::Snoozed;
        st.alarm_id = m_waiting_snooze_id;
        st.snooze_left_ms = m_waiting_snooze_until > wall ? (m_waiting_snooze_until - wall) * 1000 : 0;
    }
    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        m_status = st;
        m_status_ring_start = now - st.ringing_ms;
        m_status_snooze_end = now + st.snooze_left_ms;
    }

    const AlarmRingState state = st.state == AlarmRing::State::Ringing ? AlarmRingState::RINGING
                               : st.state == AlarmRing::State::Snoozed ? AlarmRingState::SNOOZED
                                                                       : AlarmRingState::IDLE;
    const uint32_t snooze_until = st.snooze_left_ms ? wall + st.snooze_left_ms / 1000 : 0;
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

int64_t AlarmService::checkReminders(int64_t now) {
    int64_t next = 0;
    for (auto& [id, r] : listReminders()) {
        if (!r.enabled) continue;
        const int64_t f = nextFireOf(r, std::max<int64_t>(m_checked_until, r.last_fired));
        if (f == 0) continue;
        if (f > now) {
            if (next == 0 || f < next) next = f;
            continue;
        }
        ReminderDoc upd;
        upd.last_fired = (uint32_t)now;
        upd.pending = !r.action;   // a missed action is skipped, not kept for later
        uint64_t fields = ReminderDoc::F_LAST_FIRED | ReminderDoc::F_PENDING;
        if (whenFields(r).oneShot()) {
            upd.enabled = false;
            fields |= ReminderDoc::F_ENABLED;
        } else {
            const int64_t n = nextFireOf(r, now);
            if (n && (next == 0 || n < next)) next = n;
        }
        mergeReminder(id, upd, fields);
        ESP_LOGI(TAG, "%s %d due: \"%s\"", r.action ? "Action" : "Reminder", id, r.text.c_str());
        m_reminders_due.push_back(id);
    }
    return next;
}

namespace {

// The briefing after an alarm with briefing set is stopped: a due action.
constexpr const char* BRIEFING =
    "The user just stopped their alarm. Give a short morning briefing: greet them, say the time, today's "
    "weather at home, their alarms and reminders for today (list_schedule), and three top headlines if you "
    "have a news tool. If the note 'briefing' exists, read it first and follow it instead. Under a minute.";

struct DueItem {
    std::string text;
    bool action;
};

// The first turn of the delivery session. Reminders are told to the user;
// actions are carried out and confirmed in a few words, which also marks
// the delivery done (tickDelivery waits for speech).
std::string schedulePrompt(const std::vector<DueItem>& items) {
    std::string p = "[Automatic message from the device, not spoken by the user] ";
    p += items.size() == 1 ? "An item the user scheduled is due now:" : "Items the user scheduled are due now:";
    for (size_t i = 0; i < items.size(); ++i) {
        p += "\n" + std::to_string(i + 1) + (items[i].action ? ". [action] " : ". [reminder] ") + items[i].text;
    }
    p += "\nTell the user each reminder briefly and naturally. Carry out each action with your tools, then "
         "confirm it in a few words; don't call it a reminder. If an action names a routine, read the note "
         "'routines' and follow its steps.";
    return p;
}

} // namespace

void AlarmService::deliverReminders() {
    if ((m_reminders_due.empty() && !m_briefing_due) || m_delivery != Delivery::None ||
        m_ring.state() != AlarmRing::State::Idle) {
        return;
    }
    // A session in the middle of a turn (or closing) finishes first.
    const AssistantState st = EmbeddedSysDb::getInstance().snapshot().assistant.session_state;
    if (st != AssistantState::Idle && st != AssistantState::StreamingUserAudio &&
        st != AssistantState::WaitingForFollowup) {
        return;
    }
    m_delivering.clear();
    m_delivering.swap(m_reminders_due);
    std::vector<DueItem> items;
    m_delivering_reminder = false;
    if (m_briefing_due) {
        items.push_back({BRIEFING, true});
        m_briefing_due = false;
    }
    for (int id : m_delivering) {
        ReminderDoc r;
        if (loadReminder(id, r) && !r.text.empty()) {
            items.push_back({r.text, r.action});
            m_delivering_reminder |= !r.action;
        }
    }
    if (items.empty()) {
        m_delivering.clear();
        return;
    }
    m_delivery_prompt = schedulePrompt(items);
    // One chime for all the reminders due together; the session opens after
    // it. Actions alone open it without a chime.
    ESP_LOGI(TAG, "Delivering %u scheduled item(s)", (unsigned)items.size());
    m_delivery = Delivery::Chime;
    m_delivery_start_ms = nowMs();
    m_delivery_next_ms = m_delivery_start_ms;
    if (m_delivering_reminder) {
        AlertPlayer::getInstance().playAlert(ALERT_REMINDER);
        m_delivery_next_ms += CHIME_MS;
    }
}

bool AlarmService::inScheduledTurn() const {
    return nowMs() < m_sched_until_ms && GeminiProtocol::getInstance().turnsCompleted() == m_sched_turn;
}

void AlarmService::startOfflineChimes() {
    if (!m_delivering_reminder) {
        ESP_LOGW(TAG, "Scheduled action not carried out; skipped");
        m_delivery = Delivery::None;
        m_delivering.clear();
        return;
    }
    ESP_LOGW(TAG, "Reminder not spoken; chiming and leaving it pending");
    m_delivery = Delivery::Offline;
    m_chimes_left = OFFLINE_CHIMES - 1;
    m_delivery_next_ms = nowMs() + OFFLINE_CHIME_GAP_MS;
    m_delivering.clear();
}

uint32_t AlarmService::tickDelivery() {
    if (m_delivery == Delivery::None) return m_reminders_due.empty() && !m_briefing_due ? 0 : 500;   // retry when free
    // An alarm takes over; the reminders stay pending.
    if (m_ring.state() != AlarmRing::State::Idle) {
        ESP_LOGI(TAG, "Reminder delivery cut short by an alarm");
        m_delivery = Delivery::None;
        m_delivering.clear();
        return 0;
    }
    const uint64_t now = nowMs();
    const SystemState snap = EmbeddedSysDb::getInstance().snapshot();
    switch (m_delivery) {
    case Delivery::Chime: {
        if (now < m_delivery_next_ms) break;
        const AssistantState st = snap.assistant.session_state;
        const bool open = st == AssistantState::StreamingUserAudio || st == AssistantState::WaitingForFollowup;
        // Actions alone open the session without the wake chimes.
        if (!open && !m_delivering_reminder) AssistantService::requestQuietWake();
        if (!snap.system.wifi_connected || !(open || WakeWordEngine::getInstance().requestManualWake())) {
            AssistantService::cancelQuietWake();
            startOfflineChimes();
            break;
        }
        m_sched_turn = GeminiProtocol::getInstance().turnsCompleted();
        m_sched_until_ms = now + SPEAK_TIMEOUT_MS + SCHEDULED_TURN_MS;
        GeminiProtocol::getInstance().sendTextTurn(m_delivery_prompt);
        m_delivery = Delivery::Speaking;
        m_delivery_next_ms = now + SPEAK_TIMEOUT_MS;
        break;
    }
    case Delivery::Speaking:
        if (snap.assistant.session_state == AssistantState::AssistantSpeaking) {
            ESP_LOGI(TAG, "Scheduled item spoken %llu ms after it was due", (unsigned long long)(now - m_delivery_start_ms));
            for (int id : m_delivering) acknowledgeReminder(id);
            m_delivering.clear();
            m_delivery = Delivery::None;
        } else if (now >= m_delivery_next_ms) {
            startOfflineChimes();
        }
        break;
    case Delivery::Offline:
        if (now < m_delivery_next_ms) break;
        AlertPlayer::getInstance().playAlert(ALERT_REMINDER);
        if (--m_chimes_left <= 0) {
            m_delivery = Delivery::None;
        } else {
            m_delivery_next_ms = now + OFFLINE_CHIME_GAP_MS;
        }
        break;
    default:
        break;
    }
    if (m_delivery == Delivery::None) return m_reminders_due.empty() && !m_briefing_due ? 0 : 500;
    if (m_delivery == Delivery::Speaking) return 250;
    return (uint32_t)std::max<uint64_t>(m_delivery_next_ms > now ? m_delivery_next_ms - now : 0, 1);
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
    int waiting_id = 0;
    uint32_t waiting_until = 0;
    for (auto& [id, a] : listAlarms()) {
        // A snooze saved before a reboot (the running one lives in m_ring).
        if (a.snooze_until && id != snoozed_id) {
            if (a.snooze_until > now) {
                consider(a.snooze_until);
                if (!waiting_id && !snoozed_id) {
                    waiting_id = id;
                    waiting_until = a.snooze_until;
                }
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
    const int64_t next_reminder = checkReminders(now);
    if (next_reminder) consider(next_reminder);
    m_checked_until = now;
    if (waiting_id != m_waiting_snooze_id || waiting_until != m_waiting_snooze_until) {
        m_waiting_snooze_id = waiting_id;
        m_waiting_snooze_until = waiting_until;
        publish();
    }

    if (due_id) {
        Command c{CmdType::Ring};
        c.ring.alarm_id = due_id;
        c.ring.tone = due.tone;
        c.ring.snooze_ms = (uint32_t)(due.snooze_min ? due.snooze_min : 9) * 60000;
        c.ring.volume = due.volume;
        handle(c);
    }
    deliverReminders();

    uint32_t wait_ms = MAX_IDLE_MS;
    if (next != 0) {
        // Just after the second it is due.
        const int64_t ms = (next - now) * 1000 - tv.tv_usec / 1000 + 20;
        wait_ms = (uint32_t)std::min<int64_t>(std::max<int64_t>(ms, 20), MAX_IDLE_MS);
    }
    if (const uint32_t delivery_ms = tickDelivery()) wait_ms = std::min(wait_ms, delivery_ms);
    return wait_ms;
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
