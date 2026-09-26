#include "services/alarm/AlarmRing.h"

namespace Services {

using Action = AlarmRing::Action;

Action AlarmRing::fire(int alarm_id, bool has_song, uint64_t now_ms) {
    if (m_state == State::Ringing) return Action::None;
    m_alarm_id = alarm_id;
    m_song_ok = has_song;
    m_snoozes = 0;
    m_last_end = EndReason::None;
    return startRinging(now_ms);
}

Action AlarmRing::startRinging(uint64_t now_ms) {
    m_state = State::Ringing;
    m_ring_start = now_ms;
    m_snooze_until = 0;
    if (m_song_ok) {
        m_source = Source::Song;
        m_song_start = now_ms;
        m_song_progressed = false;
        return Action::PlaySong;
    }
    m_source = Source::Builtin;
    return Action::PlayBuiltin;
}

Action AlarmRing::finish(EndReason reason) {
    m_state = State::Idle;
    m_source = Source::None;
    m_snooze_until = 0;
    m_last_end = reason;
    return Action::Finish;
}

Action AlarmRing::stop(uint64_t) {
    if (m_state == State::Idle) return Action::None;
    return finish(EndReason::Stopped);
}

Action AlarmRing::snooze(uint64_t now_ms) {
    if (m_state != State::Ringing) return Action::None;
    m_state = State::Snoozed;
    m_source = Source::None;
    m_snooze_until = now_ms + m_cfg.snooze_ms;
    ++m_snoozes;
    return Action::Silence;
}

Action AlarmRing::songProgress(uint64_t) {
    if (m_state == State::Ringing && m_source == Source::Song) m_song_progressed = true;
    return Action::None;
}

Action AlarmRing::songEnded(uint64_t now_ms) {
    if (m_state != State::Ringing || m_source != Source::Song) return Action::None;
    // Ending without ever playing is a failure, not a loop.
    if (!m_song_progressed) return songFailed(now_ms);
    m_song_start = now_ms;
    m_song_progressed = false;
    return Action::PlaySong;
}

Action AlarmRing::songFailed(uint64_t) {
    m_song_ok = false;
    if (m_state != State::Ringing || m_source != Source::Song) return Action::None;
    m_source = Source::Builtin;
    return Action::PlayBuiltin;
}

Action AlarmRing::tick(uint64_t now_ms) {
    switch (m_state) {
    case State::Idle:
        return Action::None;
    case State::Snoozed:
        if (now_ms >= m_snooze_until) return startRinging(now_ms);
        return Action::None;
    case State::Ringing:
        if (now_ms - m_ring_start >= m_cfg.ring_limit_ms) return finish(EndReason::TimedOut);
        if (m_source == Source::Song && !m_song_progressed &&
            now_ms - m_song_start >= m_cfg.start_watchdog_ms) {
            return songFailed(now_ms);
        }
        return Action::None;
    }
    return Action::None;
}

uint32_t AlarmRing::snoozeLeftMs(uint64_t now_ms) const {
    if (m_state != State::Snoozed || now_ms >= m_snooze_until) return 0;
    return static_cast<uint32_t>(m_snooze_until - now_ms);
}

uint32_t AlarmRing::ringingMs(uint64_t now_ms) const {
    if (m_state != State::Ringing || now_ms < m_ring_start) return 0;
    return static_cast<uint32_t>(now_ms - m_ring_start);
}

} // namespace Services
