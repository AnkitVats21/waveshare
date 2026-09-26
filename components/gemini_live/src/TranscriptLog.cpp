#include "gemini_live/TranscriptLog.h"

#include <algorithm>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace {
const char* const TAG = "Transcript";
}

TranscriptLog& TranscriptLog::instance() {
    static TranscriptLog log;
    return log;
}

TranscriptLog::Entry* TranscriptLog::open(Role role) {
    if (!m_ring) {
        m_ring = static_cast<Entry*>(heap_caps_calloc(MAX_ENTRIES, sizeof(Entry), MALLOC_CAP_SPIRAM));
        if (!m_ring) return nullptr;
    }
    if (m_count > 0) {
        Entry& last = m_ring[(m_next + MAX_ENTRIES - 1) % MAX_ENTRIES];
        if (!last.done && last.role == role) return &last;
        // The other side started talking: the previous entry is finished.
        last.done = true;
    }
    Entry& e = m_ring[m_next];
    m_next = (m_next + 1) % MAX_ENTRIES;
    if (m_count < MAX_ENTRIES) m_count++;
    e = {};
    e.id = m_next_id++;
    e.t_ms = esp_timer_get_time() / 1000;
    e.role = role;
    return &e;
}

bool TranscriptLog::appendLocked(Role role, const char* text, size_t len) {
    Entry* e = open(role);
    if (!e) return false;
    size_t n = std::min(len, MAX_TEXT - 1 - e->len);
    // Don't cut a multi-byte UTF-8 character in half.
    while (n < len && n > 0 && (static_cast<uint8_t>(text[n]) & 0xC0) == 0x80) n--;
    memcpy(e->text + e->len, text, n);
    e->len += n;
    e->text[e->len] = '\0';
    e->seq = ++m_seq;
    return true;
}

void TranscriptLog::append(Role role, const char* text, size_t len) {
    if (!text || len == 0) return;
    bool changed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        changed = appendLocked(role, text, len);
    }
    if (changed) notify();
}

void TranscriptLog::closeTurn() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Log the whole turn once, rather than every fragment as it arrives.
        const bool log = m_logging;
        for (size_t i = 0; i < m_count; ++i) {
            Entry& e = m_ring[(m_next + MAX_ENTRIES - m_count + i) % MAX_ENTRIES];
            if (e.id < m_turn_first_id) continue;
            if (log) ESP_LOGI(TAG, "%s: %s", e.role == Role::User ? "user" : "model", e.text);
            if (!e.done) {
                e.done = true;
                e.seq = ++m_seq;
            }
        }
        m_turn_first_id = m_next_id;
    }
    notify();
}

void TranscriptLog::toJson(uint32_t since, JsonDocument& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    out["seq"] = m_seq;
    JsonArray entries = out["entries"].to<JsonArray>();
    for (size_t i = 0; i < m_count; ++i) {
        const Entry& e = m_ring[(m_next + MAX_ENTRIES - m_count + i) % MAX_ENTRIES];
        if (e.seq <= since) continue;
        JsonObject o = entries.add<JsonObject>();
        o["id"] = e.id;
        o["seq"] = e.seq;
        o["role"] = e.role == Role::User ? "user" : "model";
        o["t_ms"] = e.t_ms;
        o["done"] = e.done;
        o["text"] = e.text;
    }
}
