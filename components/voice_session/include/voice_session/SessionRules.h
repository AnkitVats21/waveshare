#pragma once

#include <cstdint>

/**
 * @brief What a voice session does after a reply: the rules, as a pure
 * function of what happened in the session and the tunables.
 *
 * VoiceSession collects the facts (tool ends from the tool registry, the
 * hub's reports) and asks decide(); AssistantService carries the answer out.
 * Portable C++, tested on the host (host_tests/tests/test_session_rules.py).
 */
namespace voice {

// What paused music does while the session waits for a follow-up.
enum class MusicAfter : uint8_t { Hold, Quiet, Play };

struct AfterReply {
    bool end = false;              // close once the reply has played
    uint32_t window_ms = 60000;    // else: how long to wait for a follow-up
    MusicAfter music = MusicAfter::Hold;
};

// What happened so far in the session.
struct SessionFacts {
    uint8_t music_ok = 0;       // the board's music tools that succeeded
    uint8_t music_failed = 0;   // ... that answered ambiguous, not found or error
    uint8_t tools_ok = 0;       // other tools (board or MCP)
    uint8_t tools_failed = 0;
    bool hub_music_done = false;   // the hub ran a music command (end_after_reply)
};

// The tunables. Defaults are compiled in; later they come from the settings.
struct Rules {
    uint32_t followup_ms = 60000;
    // A music tool ends the session even when it failed (today's behaviour:
    // "play X" closes the session whatever happened).
    bool end_after_failed_music = true;
};

AfterReply decide(const SessionFacts& facts, const Rules& rules);

} // namespace voice
