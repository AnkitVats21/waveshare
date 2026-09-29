#pragma once

#include "common/ReactorTask.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "app/media_player/IPlaybackObserver.h"
#include "services/alarm/AlarmRing.h"
#include "services/alarm/AlarmSchedule.h"
#include "services/storage/SystemDatabase.h"
#include "freertos/semphr.h"
#include <deque>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace Services {

using AlarmDoc = ndb::system::AlarmDoc;
using ReminderDoc = ndb::system::ReminderDoc;

/**
 * @brief Alarm scheduler and ringer (docs/alarm-design.md).
 *
 * Fires the alarms in system.ndb and rings them: the device
 * is taken over (assistant session ended, chimes muted, wake word off, keys
 * captured), NexusPlayer plays the tone song looped, and the built-in tone
 * from PSRAM takes over if the song fails. Stop gives everything back and
 * resumes the music that was playing.
 *
 * All ringing decisions run on this task: requests from other tasks (keys,
 * HTTP, voice tools, NexusPlayer events) are queued and handled in order.
 */
class AlarmService : public ReactorTask, public IPlaybackObserver {
public:
    static AlarmService& getInstance();

    // Alarm volume floor while ringing (speaker volume, 0-100).
    static constexpr int MIN_VOLUME = 60;
    // Fade-in of the alarm song: from this media gain to full.
    static constexpr float FADE_FROM = 0.10f;
    static constexpr uint32_t FADE_MS = 10000;
    // A briefing alarm's music rises more gently.
    static constexpr uint32_t BRIEFING_FADE_MS = 30000;

    // Longest tone id (a CatalogDB song id).
    static constexpr size_t MAX_TONE_LEN = 63;
    // A snooze that ended while the device was off still rings if it ended
    // less than this long ago.
    static constexpr uint32_t SNOOZE_GRACE_S = 600;

    bool begin();

    // ── Alarms (system.ndb "alarms") ─────────────────────────────────────
    std::vector<std::pair<int, AlarmDoc>> alarms();
    // Writes `doc` under `id`, or under a new id if id <= 0; sets `created`
    // on a new alarm and clears last_fired / snooze_until. Returns the id, 0 on failure.
    int saveAlarm(int id, AlarmDoc doc);
    // Deletes the alarm, stopping it if it is ringing or snoozed.
    bool deleteAlarm(int id);
    // Next fire of an enabled alarm (epoch s), 0 if none.
    static int64_t nextFireOf(const AlarmDoc& doc, int64_t now);
    static AlarmWhen whenOf(const AlarmDoc& doc);

    // ── Scheduled items (system.ndb "reminders") ─────────────────────────
    // A due reminder chimes and is marked pending until it is spoken or
    // acknowledged (docs/alarm-design.md, "Delivery"). A due action (action
    // = true) opens the session without a chime and is never pending: when
    // it can't be carried out (offline), it is skipped.
    static constexpr size_t MAX_REMINDER_TEXT = 200;
    // Offline: the reminder chime this many times, this far apart.
    static constexpr int OFFLINE_CHIMES = 3;
    static constexpr uint32_t OFFLINE_CHIME_GAP_MS = 2500;
    // The session opens after the chime; if Gemini has not started speaking
    // this long after, the reminder is delivered as offline.
    static constexpr uint32_t CHIME_MS = 1000;
    static constexpr uint32_t SPEAK_TIMEOUT_MS = 25000;
    // After speech starts, the delivery turn may run this long (tool calls).
    static constexpr uint32_t SCHEDULED_TURN_MS = 60000;

    std::vector<std::pair<int, ReminderDoc>> reminders();
    // Like saveAlarm; a changed reminder is no longer pending.
    int saveReminder(int id, ReminderDoc doc);
    bool deleteReminder(int id);
    // Clears pending. False if there is no such reminder.
    bool acknowledgeReminder(int id);
    static int64_t nextFireOf(const ReminderDoc& doc, int64_t now);
    // True while Gemini answers a due item: nobody asked for that turn, so
    // the schedule tools refuse to change the schedule (a recurring routine
    // can't add or remove items unattended). Any task.
    bool inScheduledTurn() const;

    // ── Briefing music (docs/alarm-design.md, "Briefing music") ──────────
    // Played under the briefing after an alarm with briefing is stopped: full
    // until the assistant speaks, ducked to briefing_duck % while it speaks
    // and through the follow-up window, then full for MUSIC_TAIL_MS and faded
    // out; the music from before the alarm resumes after it.
    static constexpr int BRIEFING_DUCK_MIN = 10;
    static constexpr int BRIEFING_DUCK_MAX = 50;
    // Silence timeout of the briefing session (the user may reply).
    static constexpr uint32_t BRIEFING_FOLLOWUP_MS = 8000;
    static constexpr uint32_t MUSIC_UP_MS = 2000;      // back to full after the briefing
    static constexpr uint32_t MUSIC_TAIL_MS = 60000;   // then plays this long
    static constexpr uint32_t MUSIC_FADE_MS = 3000;    // and fades out
    static constexpr uint32_t MUSIC_STOP_MS = 1000;    // fade when the user takes over
    // No briefing voice this long after the music started: stop waiting.
    static constexpr uint32_t MUSIC_INTRO_MAX_MS = 60000;
    // The music starts once the session is up (its TLS handshake and the
    // music's decoding together took internal RAM to 7.7 KB), or after this.
    static constexpr uint32_t MUSIC_CONNECT_WAIT_MS = 8000;
    // Automatic briefing: the music fades in over this once the session is
    // up; after "stop" the music comes back over MUSIC_BACK_MS.
    static constexpr uint32_t AUTO_FADE_MS = 10000;
    static constexpr uint32_t MUSIC_BACK_MS = 2000;

    // ── Ringing ──────────────────────────────────────────────────────────
    struct RingOptions {
        int alarm_id = 0;          // 0 = test ring
        std::string tone;          // CatalogDB id; empty = built-in tone
        uint32_t ring_limit_ms = 0;  // 0 = default (test rings may shorten)
        uint32_t snooze_ms = 0;
        int volume = 0;            // volume floor while ringing; 0 = MIN_VOLUME
        // A briefing alarm (kind 2): rings its music (briefingTone); with
        // briefing_auto the briefing starts at once and the alarm rings after it.
        bool briefing = false;
        bool briefing_auto = false;
        bool briefed = false;      // internal: rings after an automatic briefing (none on stop)
        bool keep_music = false;   // internal: the briefing music already playing is the ring
    };
    // Each returns once the request is handled (or after timeout_ms; 0 = don't wait).
    bool ring(const RingOptions& opts, uint32_t timeout_ms = 0);
    bool stopActiveAlarm(bool restore_media = true, uint32_t timeout_ms = 0);
    bool snooze(uint32_t timeout_ms = 0);

    struct Status {
        AlarmRing::State state = AlarmRing::State::Idle;
        AlarmRing::Source source = AlarmRing::Source::None;
        AlarmRing::EndReason last_end = AlarmRing::EndReason::None;
        int alarm_id = 0;
        std::string tone;
        std::string tone_title;
        std::string fallback_reason;
        uint32_t ringing_ms = 0;
        uint32_t snooze_left_ms = 0;
        uint32_t snoozes = 0;
        bool time_synced = false;
        const char* music = "off";   // briefing music phase
    };
    Status status();

    // IPlaybackObserver: NexusPlayer events while the alarm owns it.
    void onTrackStarted(const char* songId) override {}
    void onTrackFinished(const char* songId) override;
    void onPlaybackError(const char* songId, int errorCode) override;

    // ReactorTask: stop/snooze request bits in the Alarm component (keys).
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    AlarmService();
    ~AlarmService() override = default;
    AlarmService(const AlarmService&) = delete;
    AlarmService& operator=(const AlarmService&) = delete;

    enum class CmdType : uint8_t { Ring, Stop, Snooze, SongEnded, SongFailed };
    struct Command {
        explicit Command(CmdType t = CmdType::Stop) : type(t) {}
        CmdType type;
        RingOptions ring;
        bool restore = true;
        SemaphoreHandle_t done = nullptr;
    };
    bool post(Command cmd, uint32_t timeout_ms);
    void handle(Command& cmd);

    // Carries out an AlarmRing action.
    void apply(AlarmRing::Action action);
    void takeOver();
    void giveBack();
    void startSong();
    void startBuiltin(const char* reason);
    void silence();
    void publish();

    enum class Music : uint8_t { None, Waiting, Intro, UnderVoice, Tail, Fading, Stopping, AfterSession };
    static const char* musicName(Music m);
    // Keeps the player for the briefing music if one is set (it starts once
    // the session is up); false if none.
    // path "" = no music (the soft tone rang): the phases run silently.
    void startBriefingMusic(const std::string& path, uint32_t start_ms);
    void playBriefingMusic();
    // Automatic briefing: at the alarm time, before it rings.
    void startAutoBriefing(const RingOptions& opts, bool has_song);
    // The automatic briefing is over (or snoozed): the alarm rings.
    void ringAfterBriefing(bool briefing_after_stop);
    // The alarm volume floor, for the ring and the automatic briefing.
    void raiseVolume();
    void restoreVolume();
    void convertBriefingFlags();
    // The user took over (a reply, the wake word, stop): fade out quickly.
    void stopBriefingMusicSoon(const char* why);
    // Stops it now and gives the player back (restore: resume the old music).
    void endBriefingMusic(bool restore);
    void setMusic(Music m, uint32_t for_ms = 0);
    // Advances the phases; returns ms until the next tick, 0 if no music.
    uint32_t tickBriefingMusic();

    void migrateAlarmsFile();
    // Fires what is due; returns ms until the next check.
    uint32_t checkSchedule();
    // Marks due reminders pending and queues them; returns the next reminder time (0 = none).
    int64_t checkReminders(int64_t now);
    // Starts delivering the queued reminders once no alarm is ringing or
    // snoozed and no session is busy.
    void deliverReminders();
    // Advances a delivery; returns ms until it needs the task again, 0 if none.
    uint32_t tickDelivery();
    void startOfflineChimes();
    void persistSnooze(int id, uint32_t until);

    std::mutex m_cmd_mutex;
    std::deque<Command> m_cmds;

    // Ringing state, touched only by this task (status() copies under m_status_mutex)
    AlarmRing m_ring;
    bool m_taken_over = false;
    std::string m_tone;
    std::string m_tone_title;
    std::string m_tone_path;      // "file:" tone: its full path
    std::string m_builtin_tone;   // built-in pattern name; "" = default
    std::string m_fallback_reason;
    int m_min_volume = MIN_VOLUME;
    bool m_ring_briefing = false;   // the ringing alarm briefs when it is stopped
    uint32_t m_fade_ms = FADE_MS;
    bool m_keep_music = false;      // startSong: the music is already playing
    int m_saved_volume = -1;
    int m_raised_volume = -1;
    // Alarms due in (m_checked_until, now] fire; 0 until the clock is set.
    int64_t m_checked_until = 0;
    bool m_warned_unsynced = false;
    // Snooze written to the alarm's document, so it survives a reboot.
    int m_snooze_saved_id = 0;
    uint32_t m_snooze_saved_until = 0;
    // A snooze saved before a restart that has not ended yet; reported as
    // snoozed, and Stop cancels it. It rings from checkSchedule().
    int m_waiting_snooze_id = 0;
    uint32_t m_waiting_snooze_until = 0;
    // Reminders due and not yet delivered (this task only).
    std::vector<int> m_reminders_due;
    enum class Delivery : uint8_t { None, Chime, Speaking, Offline };
    Delivery m_delivery = Delivery::None;
    std::vector<int> m_delivering;     // ids being delivered
    bool m_delivering_reminder = false; // at least one of them is a reminder (not an action)
    bool m_briefing_due = false;        // an alarm with briefing was stopped; delivered as an action
    bool m_delivering_briefing = false; // the delivery carries the briefing
    Music m_music = Music::None;
    std::string m_music_path;           // "" = silent (no music, the soft tone rang)
    uint32_t m_music_start_ms = 0;      // where the music resumes
    bool m_auto_pending = false;        // automatic briefing: the alarm rings after it
    RingOptions m_auto_ring;
    uint64_t m_music_until_ms = 0;      // end of the current phase
    bool m_briefing_spoken = false;     // the briefing's reply has played out
    // The model turn answering a delivery: turnsCompleted() when it was
    // sent, until the deadline (inScheduledTurn).
    std::atomic<uint32_t> m_sched_turn{0};
    std::atomic<uint64_t> m_sched_until_ms{0};
    std::string m_delivery_prompt;
    uint64_t m_delivery_start_ms = 0;
    uint64_t m_delivery_next_ms = 0;   // next step (Chime, Offline) or deadline (Speaking)
    int m_chimes_left = 0;
    std::mutex m_status_mutex;
    Status m_status;
    // For the live ringing_ms / snooze_left_ms in status()
    uint64_t m_status_ring_start = 0;
    uint64_t m_status_snooze_end = 0;

    static constexpr const char* TAG = "AlarmSvc";
};

} // namespace Services
