#pragma once

#include <cstdint>
#include <string>

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
    // A music command that failed (not found, ambiguous, an error) also
    // ends the session. Off: the session stays open so the person can
    // answer ("which one?", "try another song"), as with the hub's music
    // tools, which end the session only on success.
    bool end_after_failed_music = false;
    // After a quota error the next sessions use this model, for
    // fallback_minutes (Gemini's quotas are per model). Empty: no fallback.
    std::string fallback_model = "gemini-3.1-flash-live-preview";
    uint32_t fallback_minutes = 60;
};

AfterReply decide(const SessionFacts& facts, const Rules& rules);

// Why a session failed, from the close code and reason the backend saw
// (Gemini's own, or the hub's report of it).
enum class ErrorKind : uint8_t { None, Quota, Auth, Model, Network, Other };
const char* errorKindName(ErrorKind kind);
// code: the WebSocket close code (-1: none, the connection dropped).
ErrorKind classifyError(int code, const char* reason);

// The model for the next session: `primary`, or the fallback while one is
// active. Model names with or without the "models/" prefix.
std::string pickModel(const std::string& primary, bool fallback_active, const Rules& rules);

} // namespace voice
