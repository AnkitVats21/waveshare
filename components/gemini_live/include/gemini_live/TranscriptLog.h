#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

#include <ArduinoJson.h>

/**
 * @brief The last few turns of the conversation as text, from Gemini's input
 * and output audio transcriptions. Fragments of one turn are joined into one
 * entry. Storage is a fixed PSRAM ring allocated on first use.
 */
class TranscriptLog {
public:
    enum class Role : uint8_t { User, Model };

    static TranscriptLog& instance();

    // Appends a transcription fragment to the open entry for this role, or
    // starts a new entry.
    void append(Role role, const char* text, size_t len);
    // Ends the current turn and logs it: the next fragment starts a new entry.
    void closeTurn();

    // Entries changed after `since` (a value of "seq" from an earlier call):
    // {"seq": N, "entries": [{"id", "seq", "role", "t_ms", "done", "text"}]}
    void toJson(uint32_t since, JsonDocument& out);

    // Called after every change, outside the lock, on the Gemini task. Keep it
    // short (set a flag, notify a task).
    using ChangeCallback = void (*)(void* ctx);
    void setChangeCallback(ChangeCallback cb, void* ctx) { m_cb_ctx = ctx; m_cb = cb; }

private:
    static constexpr size_t MAX_ENTRIES = 32;
    static constexpr size_t MAX_TEXT = 1024;

    struct Entry {
        uint32_t id;
        uint32_t seq;   // last change
        int64_t t_ms;   // uptime at the first fragment
        Role role;
        bool done;
        uint16_t len;
        char text[MAX_TEXT];
    };

    TranscriptLog() = default;
    Entry* open(Role role);
    bool appendLocked(Role role, const char* text, size_t len);

    std::mutex m_mutex;
    Entry* m_ring = nullptr;
    size_t m_count = 0;    // valid entries, oldest at (m_next - m_count)
    size_t m_next = 0;     // slot for the next new entry
    uint32_t m_next_id = 1;
    uint32_t m_turn_first_id = 1;  // first entry of the turn in progress
    uint32_t m_seq = 0;
    ChangeCallback m_cb = nullptr;
    void* m_cb_ctx = nullptr;
    void notify() { if (m_cb) m_cb(m_cb_ctx); }
};
