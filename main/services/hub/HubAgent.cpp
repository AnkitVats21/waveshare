#include "services/hub/HubAgent.h"

#include "services/hub/HubLink.h"
#include "gemini_live/GeminiProtocol.h"
#include "gemini_live/PsramAllocator.h"
#include "gemini_live/TranscriptLog.h"
#include "common/AppLogger.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include "services/BufferManager.h"
#include "app/audio/AudioOrchestrator.h"
#include "app/audio/SpeakerPlayback.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <cstring>

namespace Services {

static auto& sysdb = EmbeddedSysDb::getInstance();

HubAgent& HubAgent::getInstance() {
    static HubAgent s_instance;
    return s_instance;
}

HubAgent::HubAgent()
    : ReactorTask({
          "hub_agent",
          STACK_SIZE,
          ThreadConfig::Priority::GEMINI_PROTOCOL,
          ThreadConfig::CORE_NETWORK,
          COMP::ASSISTANT | COMP::SYSTEM
      })
{
    m_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(std::string*));
    m_mic_frame = static_cast<uint8_t*>(heap_caps_malloc(MIC_FRAME_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(m_rx_queue && m_mic_frame);
    HubLink::getInstance().setVoiceListener({onMessage, onAudio, onLinkChange, this});
}

// ─────────────────────────────────────────────────────────────────────────────
// Session
// ─────────────────────────────────────────────────────────────────────────────

void HubAgent::onStateChanged(ComponentMask, const SystemState& snap) {
    if (!isActive()) return;
    auto state = snap.assistant.session_state;
    auto ws = snap.assistant.ws_state;
    // The hub was away at the wake: fail once AssistantService is in
    // Connecting, so its ErrorCooldown (and the error chime) isn't
    // overwritten by that transition.
    if (m_fail_pending) {
        if (state == AssistantState::Connecting) {
            m_fail_pending = false;
            failSession("the hub is not available");
        } else if (state == AssistantState::Idle || state == AssistantState::ErrorCooldown) {
            m_fail_pending = false;
        }
        return;
    }
    // The wake asks for the connection; ws_state must leave DISCONNECTED at
    // once (Connecting reads DISCONNECTED as a failed connect).
    if (snap.assistant.connect_requested && !m_session &&
        (ws == WsState::DISCONNECTED || ws == WsState::ERROR_STATE)) {
        if (!HubLink::getInstance().voiceAvailable()) {
            m_fail_pending = true;
            sysdb.mutate([](SystemState& s) { s.assistant.ws_state = WsState::CONNECTING; });
            return;
        }
        startSession();
    }
}

void HubAgent::startSession() {
    auto cfg = GeminiProtocol::getInstance().settings();
    m_record_transcripts = cfg.transcripts;

    // {"setup": ...} from the builder, the rest added beside it (a copy of
    // the setup into another document recursed off the end of the stack).
    PsramAllocator psram;
    JsonDocument doc(&psram);
    if (!GeminiProtocol::buildSessionSetup(doc, cfg)) {
        failSession("no setup");
        return;
    }
    doc["type"] = "session_start";
    doc["web_search"] = cfg.web_search;
    doc["keepalive_s"] = cfg.keepalive_s;
    doc["resume_min"] = cfg.resume_min;
    std::string out;
    serializeJson(doc, out);

    m_ready = false;
    m_dropping = false;
    m_reply_audio = false;
    m_accept_audio = true;
    m_session = true;
    sysdb.mutate([](SystemState& s) { s.assistant.ws_state = WsState::CONNECTING; });
    if (!sendJson(out)) {
        failSession("session_start not sent");
        return;
    }
    LOGI_NET("Hub session started (setup %u bytes)", (unsigned)out.size());
}

void HubAgent::endSession() {
    if (!m_session.exchange(false)) return;
    m_ready = false;
    m_accept_audio = false;
    {
        std::lock_guard<std::mutex> lock(m_turn_mutex);
        m_pending_turn.clear();
        m_reply_wait_us = 0;
    }
    sendJson("{\"type\":\"session_end\"}");
    sysdb.mutate([](SystemState& s) {
        s.assistant.ws_state = WsState::DISCONNECTED;
        s.audio.assistant_speaking = false;
    });
}

// The session can't go on: error chime, back to idle (AssistantService).
void HubAgent::failSession(const char* why) {
    LOGW_NET("Hub session failed: %s", why);
    bool was = m_session.exchange(false);
    m_ready = false;
    m_accept_audio = false;
    BufferManager::getInstance().flush(Buffers::VOICE_RX_BUF);
    {
        std::lock_guard<std::mutex> lock(m_turn_mutex);
        m_pending_turn.clear();
        m_reply_wait_us = 0;
    }
    if (was) sendJson("{\"type\":\"session_end\"}");
    sysdb.mutate([](SystemState& s) {
        s.assistant.ws_state = WsState::ERROR_STATE;
        s.audio.assistant_speaking = false;
        s.assistant.connect_requested = false;
        if (s.assistant.session_state != AssistantState::Idle) {
            s.assistant.session_state = AssistantState::ErrorCooldown;
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// VoiceAgent
// ─────────────────────────────────────────────────────────────────────────────

bool HubAgent::sendJson(const std::string& text) {
    return HubLink::getInstance().sendText(text);
}

void HubAgent::sendMicAudio(const uint8_t* pcm, size_t len) {
    if (!m_ready || !pcm || len == 0 || len > MIC_FRAME_MAX - 2) return;
    // Half-duplex: no uplink while the assistant speaks, unless barge-in.
    if (!m_barge_in && sysdb.assistantSpeaking()) return;
    m_mic_frame[0] = CHANNEL_MIC;
    m_mic_frame[1] = 0;
    std::memcpy(m_mic_frame + 2, pcm, len);
    // esp_websocket_client drops the link when a write times out, so wait
    // rather than give up: a late chunk costs less than the session. Over a
    // music download a write once stalled past 1 s (docs/known-issues.md).
    const int64_t t0 = esp_timer_get_time();
    const bool sent = HubLink::getInstance().sendBinary(m_mic_frame, len + 2, MIC_SEND_TIMEOUT_MS);
    const int64_t took_ms = (esp_timer_get_time() - t0) / 1000;
    if (!sent) {
        LOGW_NET("Mic chunk not sent to the hub (%d ms)", (int)took_ms);
    } else if (took_ms > 300) {
        LOGW_NET("Slow mic write to the hub: %d ms", (int)took_ms);
    }
}

void HubAgent::sendTextTurn(const std::string& text) {
    std::lock_guard<std::mutex> lock(m_turn_mutex);
    m_reply_wait_us = esp_timer_get_time();
    if (!m_ready) {
        m_pending_turn = text;
        return;
    }
    JsonDocument doc;
    doc["type"] = "text_turn";
    doc["text"] = text;
    std::string out;
    serializeJson(doc, out);
    LOGI_NET("Sending a text turn (%u chars)", (unsigned)text.size());
    sendJson(out);
}

void HubAgent::sendToolResponse(const char* call_id, const char* json_result) {
    if (!call_id || !m_session) return;
    // The result is a JSON object; spliced in as it is. (serializeJson
    // replaces a std::string's content, so the id is quoted separately.)
    JsonDocument id;
    id.set(call_id);
    std::string quoted_id;
    serializeJson(id, quoted_id);
    std::string out = "{\"type\":\"tool_response\",\"id\":" + quoted_id + ",\"result\":";
    JsonDocument check;
    out += (json_result && !deserializeJson(check, json_result) && check.is<JsonObject>()) ? json_result : "{}";
    out += "}";
    if (!sendJson(out)) LOGE_NET("Tool response %s not sent to the hub", call_id);
}

bool HubAgent::awaitingReply() {
    std::lock_guard<std::mutex> lock(m_turn_mutex);
    return m_reply_wait_us != 0 && esp_timer_get_time() - m_reply_wait_us < REPLY_WAIT_US;
}

void HubAgent::expectReply() {
    std::lock_guard<std::mutex> lock(m_turn_mutex);
    m_reply_wait_us = esp_timer_get_time();
}

void HubAgent::interruptReply() {
    auto snap = sysdb.snapshot();
    if (!snap.audio.assistant_speaking) return;
    // The hub drops what it queued and the rest of the reply; audio already
    // on its way is dropped here until the turn ends.
    bool arriving = !snap.audio.turn_complete_pending;
    if (arriving) m_dropping = true;
    BufferManager::getInstance().flush(Buffers::VOICE_RX_BUF);
    sysdb.mutate([](SystemState& s) {
        if (s.audio.assistant_speaking) s.audio.turn_complete_pending = true;
    });
    expectReply();   // to what the person says next; holds the silence timeout
    sendJson("{\"type\":\"interrupt\"}");
    LOGI_NET("Interrupting the reply%s.", arriving ? "" : " (fully received; flushed)");
}

// ─────────────────────────────────────────────────────────────────────────────
// From the hub
// ─────────────────────────────────────────────────────────────────────────────

void HubAgent::onMessage(const std::string& text, void* ctx) {
    auto* self = static_cast<HubAgent*>(ctx);
    auto* msg = new std::string(text);
    if (xQueueSend(self->m_rx_queue, &msg, 0) != pdTRUE) {
        LOGW_NET("Hub voice queue full; dropped a message");
        delete msg;
        return;
    }
    xTaskNotify(self->getHandle(), BIT_RX, eSetBits);
}

void HubAgent::onLinkChange(bool, void* ctx) {
    auto* self = static_cast<HubAgent*>(ctx);
    if (self->getHandle()) xTaskNotify(self->getHandle(), BIT_LINK, eSetBits);
}

// The client's task (3.5 KB internal stack): copy into VOICE_RX_BUF and return.
void HubAgent::onAudio(uint8_t channel, const uint8_t* data, size_t len, bool start, void* ctx) {
    auto* self = static_cast<HubAgent*>(ctx);
    if (channel != CHANNEL_VOICE || !self->m_accept_audio || self->m_dropping || len == 0) return;
    auto& bm = BufferManager::getInstance();
    if (!self->m_reply_audio.exchange(true)) {
        bm.flush(Buffers::VOICE_RX_BUF);   // a stale reply's tail
        if (self->getHandle()) xTaskNotify(self->getHandle(), BIT_REPLY_STARTED, eSetBits);
    }
    if (start) {
        const int64_t now = esp_timer_get_time();
        const int64_t last = self->m_reply_last_us.exchange(now);
        if (self->m_reply_first_us.load() == 0) {
            self->m_reply_first_us = now;
        } else if (last && now - last > self->m_reply_max_gap_us.load()) {
            self->m_reply_max_gap_us = now - last;
        }
    }
    self->m_reply_bytes += len;
    // The hub sends at most ~1.5 s ahead and the buffer holds ~5 s: a full
    // buffer means playback stalled. Never block the link.
    if (!bm.send(Buffers::VOICE_RX_BUF, data, len, pdMS_TO_TICKS(VOICE_SEND_WAIT_MS))) {
        self->m_reply_dropped += len;
    }
}

void HubAgent::replyStarted() {
    {
        std::lock_guard<std::mutex> lock(m_turn_mutex);
        m_reply_wait_us = 0;
    }
    if (!m_accept_audio || sysdb.assistantSpeaking()) return;
    sysdb.mutate([](SystemState& s) {
        s.assistant.session_state = AssistantState::AssistantSpeaking;
        s.assistant.visual_state  = AssistantVisualState::Speaking;
        s.audio.assistant_speaking = true;
    });
    AudioOrchestrator::getInstance().notifyVoiceStarted();
}

// Logs the reply's delivery, like GeminiProtocol's line (24 kHz 16-bit mono
// is 48 bytes per ms).
void HubAgent::replyEnded(const char* how) {
    m_reply_audio = false;
    const int64_t first = m_reply_first_us.exchange(0);
    const int64_t last = m_reply_last_us.exchange(0);
    const int64_t gap = m_reply_max_gap_us.exchange(0);
    const uint32_t bytes = m_reply_bytes.exchange(0);
    const uint32_t dropped = m_reply_dropped.exchange(0);
    if (bytes == 0) return;
    const uint32_t audio_ms = bytes / 48;
    const uint32_t span_ms = (uint32_t)((last - first) / 1000);
    LOGI_NET("Reply delivery (hub, %s): ~%u ms audio over %u ms (%u.%02ux real time), largest gap %u ms, "
             "dropped %u ms",
             how, (unsigned)audio_ms, (unsigned)span_ms,
             span_ms ? (unsigned)(audio_ms / span_ms) : 0u,
             span_ms ? (unsigned)(audio_ms * 100 / span_ms % 100) : 0u,
             (unsigned)(gap / 1000), (unsigned)(dropped / 48));
}

void HubAgent::handleMessage(const std::string& text) {
    JsonDocument doc;
    if (deserializeJson(doc, text)) {
        LOGW_NET("Unreadable hub message (%u bytes)", (unsigned)text.size());
        return;
    }
    const char* type = doc["type"] | "";
    if (!m_session) {
        return;   // a late message of a session already over
    }

    if (strcmp(type, "session_ready") == 0) {
        m_ready = true;
        LOGI_NET("Hub session ready%s", (doc["resumed"] | false) ? " (conversation resumed)" : "");
        sysdb.mutate([](SystemState& s) { s.assistant.ws_state = WsState::CONNECTED; });
        std::string pending;
        {
            std::lock_guard<std::mutex> lock(m_turn_mutex);
            pending.swap(m_pending_turn);
        }
        if (!pending.empty()) sendTextTurn(pending);
    } else if (strcmp(type, "transcription") == 0) {
        const char* role = doc["role"] | "";
        const char* t = doc["text"] | "";
        bool user = strcmp(role, "user") == 0;
        // Gemini heard the person, so a reply follows (possibly seconds later).
        if (user) expectReply();
        if (t[0] && m_record_transcripts) {
            TranscriptLog::instance().append(user ? TranscriptLog::Role::User : TranscriptLog::Role::Model,
                                             t, strlen(t));
        }
    } else if (strcmp(type, "reply_owed") == 0) {
        expectReply();   // an MCP tool is running on the hub
    } else if (strcmp(type, "tool_call") == 0) {
        const char* id = doc["id"] | "";
        const char* tool = doc["name"] | "";
        if (id[0] && dispatchToolCall(id, tool, doc["args"].as<JsonObjectConst>())) {
            expectReply();   // the model answers after our tool_response
        }
    } else if (strcmp(type, "turn_complete") == 0) {
        LOGI_NET("Assistant turn complete");
        replyEnded("complete");
        m_dropping = false;
        m_turns_completed++;
        {
            std::lock_guard<std::mutex> lock(m_turn_mutex);
            m_reply_wait_us = 0;
        }
        TranscriptLog::instance().closeTurn();
        sysdb.mutate([](SystemState& s) { s.audio.turn_complete_pending = true; });
    } else if (strcmp(type, "interrupted") == 0) {
        // Barge-in heard by Gemini: drop what is queued; the drain after
        // turn_complete_pending ends the voice.
        LOGI_NET("Reply interrupted by the user.");
        replyEnded("interrupted");
        m_dropping = false;
        BufferManager::getInstance().flush(Buffers::VOICE_RX_BUF);
        sysdb.mutate([](SystemState& s) {
            if (s.audio.assistant_speaking) s.audio.turn_complete_pending = true;
        });
    } else if (strcmp(type, "session_error") == 0) {
        LOGW_NET("Hub session error: %s", doc["reason"] | "?");
        failSession("the hub ended the session");
    } else {
        LOGD_NET("Ignoring hub message '%s'", type);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Task
// ─────────────────────────────────────────────────────────────────────────────

void HubAgent::run() {
    while (m_running) {
        uint32_t bits = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY) != pdTRUE) continue;

        if (bits & BIT_REPLY_STARTED) replyStarted();
        if (bits & BIT_RX) {
            std::string* msg = nullptr;
            while (xQueueReceive(m_rx_queue, &msg, 0) == pdTRUE) {
                handleMessage(*msg);
                delete msg;
            }
        }
        if ((bits & BIT_LINK) && m_session && !HubLink::getInstance().isUp()) {
            failSession("the hub link dropped");
        }
        uint32_t db_changed = bits & ~(BIT_RX | BIT_REPLY_STARTED | BIT_LINK);
        if (db_changed) {
            m_last_changed = db_changed;
            onStateChanged(db_changed, sysdb.snapshot());
        }
    }
}

} // namespace Services
