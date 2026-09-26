#pragma once

#include <cstdint>

namespace Services {

/**
 * @brief What a ringing alarm does, as a state machine with no I/O.
 *
 * AlarmService feeds it events and carries out the returned action; the clock
 * (milliseconds, monotonic) is passed in, so ring limits, snooze and the
 * fallback to the built-in tone are host-tested without waiting.
 *
 *   IDLE --fire--> RINGING --snooze--> SNOOZED --snooze ends--> RINGING
 *                     |  stop / ring limit                         |
 *   IDLE <------------+--------------------------------------------+ (stop)
 *
 * A song that fails, or makes no progress within START_WATCHDOG of starting,
 * is replaced by the built-in tone for the rest of that alarm, snoozes included.
 */
class AlarmRing {
public:
    enum class State : uint8_t { Idle, Ringing, Snoozed };
    enum class Source : uint8_t { None, Song, Builtin };
    enum class EndReason : uint8_t { None, Stopped, TimedOut };

    enum class Action : uint8_t {
        None,
        PlaySong,     // start (or restart) the alarm song from the beginning
        PlayBuiltin,  // stop the song if any, loop the built-in tone
        Silence,      // stop the sound, keep the device taken over (snooze)
        Finish,       // stop the sound, give the device back
    };

    struct Config {
        uint32_t ring_limit_ms = 10 * 60 * 1000;
        uint32_t snooze_ms = 9 * 60 * 1000;
        uint32_t start_watchdog_ms = 3000;
    };

    AlarmRing() = default;
    explicit AlarmRing(const Config& cfg) : m_cfg(cfg) {}
    void setConfig(const Config& cfg) { m_cfg = cfg; }
    const Config& config() const { return m_cfg; }

    // An alarm is due. Ignored while ringing; replaces a snoozed alarm.
    Action fire(int alarm_id, bool has_song, uint64_t now_ms);
    Action stop(uint64_t now_ms);
    Action snooze(uint64_t now_ms);

    // Song playback reports.
    Action songProgress(uint64_t now_ms);
    Action songEnded(uint64_t now_ms);
    Action songFailed(uint64_t now_ms);

    // Periodic check: watchdog, ring limit, end of snooze.
    Action tick(uint64_t now_ms);

    State state() const { return m_state; }
    Source source() const { return m_source; }
    int alarmId() const { return m_alarm_id; }
    EndReason lastEnd() const { return m_last_end; }
    uint32_t snoozeCount() const { return m_snoozes; }
    // Milliseconds of snooze left, 0 when not snoozed.
    uint32_t snoozeLeftMs(uint64_t now_ms) const;
    // Milliseconds rung so far in this ring (since fire or the end of a snooze).
    uint32_t ringingMs(uint64_t now_ms) const;

private:
    Action startRinging(uint64_t now_ms);
    Action finish(EndReason reason);

    Config m_cfg;
    State m_state = State::Idle;
    Source m_source = Source::None;
    int m_alarm_id = 0;
    bool m_song_ok = false;        // a song is set and has not failed this alarm
    bool m_song_progressed = false;
    uint64_t m_ring_start = 0;
    uint64_t m_song_start = 0;
    uint64_t m_snooze_until = 0;
    uint32_t m_snoozes = 0;
    EndReason m_last_end = EndReason::None;
};

} // namespace Services
