#pragma once

#include "common/ReactorTask.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "app/media_player/IPlaybackObserver.h"
#include "services/alarm/AlarmRing.h"
#include "services/alarm/AlarmSchedule.h"
#include "services/storage/SystemDatabase.h"
#include "freertos/semphr.h"
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace Services {

using AlarmDoc = ndb::system::AlarmDoc;

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

    // ── Ringing ──────────────────────────────────────────────────────────
    struct RingOptions {
        int alarm_id = 0;          // 0 = test ring
        std::string tone;          // CatalogDB id; empty = built-in tone
        uint32_t ring_limit_ms = 0;  // 0 = default (test rings may shorten)
        uint32_t snooze_ms = 0;
        int volume = 0;            // volume floor while ringing; 0 = MIN_VOLUME
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

    void migrateAlarmsFile();
    // Fires what is due; returns ms until the next check.
    uint32_t checkSchedule();
    void persistSnooze(int id, uint32_t until);

    std::mutex m_cmd_mutex;
    std::deque<Command> m_cmds;

    // Ringing state, touched only by this task (status() copies under m_status_mutex)
    AlarmRing m_ring;
    bool m_taken_over = false;
    std::string m_tone;
    std::string m_tone_title;
    std::string m_fallback_reason;
    int m_min_volume = MIN_VOLUME;
    int m_saved_volume = -1;
    int m_raised_volume = -1;
    // Alarms due in (m_checked_until, now] fire; 0 until the clock is set.
    int64_t m_checked_until = 0;
    bool m_warned_unsynced = false;
    // Snooze written to the alarm's document, so it survives a reboot.
    int m_snooze_saved_id = 0;
    uint32_t m_snooze_saved_until = 0;
    std::mutex m_status_mutex;
    Status m_status;
    // For the live ringing_ms / snooze_left_ms in status()
    uint64_t m_status_ring_start = 0;
    uint64_t m_status_snooze_end = 0;

    static constexpr const char* TAG = "AlarmSvc";
};

} // namespace Services
