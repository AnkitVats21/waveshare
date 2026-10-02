#include "GeminiProtocol.h"
#include "GeminiAudioPump.h"
#include "gemini_skills_generated.h"
#include "common/AppLogger.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include "sd_storage/Fs.h"
#include "credentials/Credentials.h"
#include "gemini_live/TranscriptLog.h"
#include <ArduinoJson.h>
#include "gemini_live/PsramAllocator.h"
#include "sdkconfig.h"
#include "esp_timer.h"

#include "mbedtls/base64.h"
#include "services/BufferManager.h"
#include "app/audio/MicCapture.h"
#include "app/audio/SpeakerPlayback.h"
#include "app/audio/AudioOrchestrator.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include <algorithm>
#include <string>
#include <cstring>
#include <vector>

static const char* const GEMINI_LIVE_BASE_URL = "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=";
static constexpr size_t STATIC_PCM_ARENA_MAX_SIZE = 65536; // 64KB ceiling

static auto& sysdb = EmbeddedSysDb::getInstance();

// Google Search in a Live session: the 2.5 Live models have it; the 3.x Live
// models refuse it (the connection closes with "quota" before
// setupComplete). Models without it use the MCP web_search tool.
static bool modelHasGoogleSearch(const std::string& model) {
    return model.find("2.5") != std::string::npos;
}

// ─────────────────────────────────────────────────────────────────────────────
// Construction & Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

GeminiProtocol::GeminiProtocol()
    : ReactorTask({
          "gemini_proto",
          ThreadConfig::StackSize::STACK_GEMINI,
          ThreadConfig::Priority::GEMINI_PROTOCOL,
          ThreadConfig::CORE_NETWORK,
          COMP::ASSISTANT | COMP::SYSTEM
      })
{
    // Boot-time allocation of PSRAM scrap arena
    m_static_pcm_scratch_arena = static_cast<uint8_t*>(
        heap_caps_malloc(STATIC_PCM_ARENA_MAX_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    assert(m_static_pcm_scratch_arena != nullptr);

    m_static_payload_arena = static_cast<char*>(
        heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    assert(m_static_payload_arena != nullptr);

    m_assembly_scratch = static_cast<uint8_t*>(
        heap_caps_malloc(MAX_INCOMING_FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    assert(m_assembly_scratch != nullptr);

    m_incoming_psram_rb = xRingbufferCreateWithCaps(PSRAM_RB_SIZE, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(m_incoming_psram_rb != nullptr);

    esp_timer_create_args_t park_args = {};
    park_args.callback = [](void* arg) {
        auto* self = static_cast<GeminiProtocol*>(arg);
        if (self->getHandle()) xTaskNotify(self->getHandle(), NOTIFY_PARK_EXPIRED_BIT, eSetBits);
    };
    park_args.arg = this;
    park_args.dispatch_method = ESP_TIMER_TASK;
    park_args.name = "gemini_keepalive";
    ESP_ERROR_CHECK(esp_timer_create(&park_args, &m_park_timer));
}

GeminiProtocol::~GeminiProtocol() {
    if (m_incoming_psram_rb) {
        vRingbufferDelete(m_incoming_psram_rb);
    }
    if (m_assembly_scratch) {
        heap_caps_free(m_assembly_scratch);
    }
    if (m_static_pcm_scratch_arena) {
        heap_caps_free(m_static_pcm_scratch_arena);
    }
    if (m_static_payload_arena) {
        heap_caps_free(m_static_payload_arena);
    }
}

GeminiProtocol& GeminiProtocol::getInstance() {
    static GeminiProtocol instance;
    return instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReactorTask Interface
// ─────────────────────────────────────────────────────────────────────────────

void GeminiProtocol::onStateChanged(ComponentMask changed, const SystemState& snap) {
    maybeHandOff(snap);
    bool requested = snap.assistant.connect_requested;
    bool wifi_ok = snap.system.wifi_connected;
    auto ws = snap.assistant.ws_state;

    // A kept connection: reuse it for the new session, or drop it once it
    // has died (else the client would keep reconnecting while idle).
    if (m_parked) {
        if (ws != WsState::CONNECTED) {
            LOGI_NET("The kept connection closed.");
            closeConnection();
            ws = WsState::DISCONNECTED;
        } else if (requested) {
            esp_timer_stop(m_park_timer);
            m_parked = false;
            m_accept_audio = true;
            LOGI_NET("Reusing the open connection.");
        }
    }

    if (requested && wifi_ok && (ws == WsState::DISCONNECTED || ws == WsState::ERROR_STATE)) {
        if (ensureClientInitialized()) {
            startClientConnection();
        } else {
            LOGE_NET("WebSocket client initialization failed.");
            // Write disconnected state directly into SysDb — no EventBus
            sysdb.mutate([](SystemState& s) {
                s.assistant.ws_state = WsState::ERROR_STATE;
                s.assistant.connect_requested = false;
            });
        }
    } else if (!wifi_ok) {
        if (ws != WsState::DISCONNECTED) {
            closeConnection();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket Client Management
// ─────────────────────────────────────────────────────────────────────────────

bool GeminiProtocol::ensureClientInitialized() {
    std::lock_guard<std::mutex> lock(m_client_mutex);
    if (m_client) {
        return true;
    }

    std::string api_key = credentials::geminiApiKey();
    if (!api_key.empty()) {
        LOGI_NET("Using the Gemini API key from NVS.");
    }

    if (api_key.empty()) {
#ifdef CONFIG_GEMINI_API_KEY
        if (std::strlen(CONFIG_GEMINI_API_KEY) == 0) {
            LOGE_NET("CONFIG_GEMINI_API_KEY is empty and no key is stored in NVS.");
            return false;
        }
        api_key = CONFIG_GEMINI_API_KEY;
#else
        LOGE_NET("CONFIG_GEMINI_API_KEY is missing and no key is stored in NVS.");
        return false;
#endif
    }
    m_ws_uri = GEMINI_LIVE_BASE_URL;
    m_ws_uri += api_key;

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = m_ws_uri.c_str();
    ws_cfg.buffer_size = 16384;
    ws_cfg.reconnect_timeout_ms = 10000;
    ws_cfg.network_timeout_ms = 10000;
    ws_cfg.task_stack = 10240;
    ws_cfg.task_prio = ThreadConfig::Priority::GEMINI_PROTOCOL;
    ws_cfg.task_core_id = ThreadConfig::CORE_NETWORK;
    ws_cfg.task_core_id_set = true;
    // The API key travels in the URL: always verify the server certificate and
    // hostname (attaching the bundle overrides CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY).
    ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    ws_cfg.skip_cert_common_name_check = false;

    if (!m_client.init(ws_cfg)) {
        LOGE_NET("Failed to initialize Gemini WebSocket client handle.");
        return false;
    }

    m_client.registerEvents(websocketEventHandler, this);
    LOGI_NET("Gemini WebSocket client initialized.");
    return true;
}

bool GeminiProtocol::startClientConnection() {
    if (!m_client) return false;

    LOGI_NET("Connecting WebSocket client...");
    
    // Stop first to ensure any asynchronous disconnect events are cleared before mutating state to CONNECTING
    m_client.stop();
    
    sysdb.mutate([](SystemState& s) {
        s.assistant.ws_state = WsState::CONNECTING;
    });

    esp_err_t err = m_client.start();
    if (err != ESP_OK) {
        LOGE_NET("esp_websocket_client_start failed: 0x%x", (unsigned)err);
        sysdb.mutate([](SystemState& s) {
            s.assistant.ws_state = WsState::ERROR_STATE;
            s.assistant.connect_requested = false;
        });
        return false;
    }

    return true;
}

void GeminiProtocol::requestRestart(bool graceful) {
    m_restart_graceful = graceful;
    m_restart_requested = true;
    sysdb.mutate([](SystemState& s) { s.assistant.ws_state = WsState::CONNECTING; });
    xTaskNotify(getHandle(), NOTIFY_RESTART_BIT, eSetBits);
}

// Protocol task only (stop() may not run on the websocket task).
void GeminiProtocol::restartConnection() {
    std::lock_guard<std::mutex> lock(m_client_mutex);
    if (!m_client || !m_accept_audio) return;   // the session ended meanwhile
    m_restarting = true;
    if (m_restart_graceful) m_client.close(pdMS_TO_TICKS(500));
    m_client.stop();
    {
        std::lock_guard<std::mutex> turn(m_turn_mutex);
        m_setup_complete = false;
    }
    if (m_interrupt_pending) {
        // Frames of the interrupted reply still queued for the parser (this
        // task) belong to the old connection: discard them.
        size_t size;
        void* item;
        while ((item = xRingbufferReceive(m_incoming_psram_rb, &size, 0)) != nullptr) {
            vRingbufferReturnItem(m_incoming_psram_rb, item);
        }
        m_interrupt_pending = false;
    }
    m_restarting = false;
    LOGI_NET("Reconnecting...");
    if (m_client.start() != ESP_OK) {
        LOGE_NET("Reconnect failed to start.");
        sysdb.mutate([](SystemState& s) { s.assistant.ws_state = WsState::ERROR_STATE; });
    }
}

void GeminiProtocol::maybeHandOff(const SystemState& snap) {
    if (!m_handoff_pending || !m_accept_audio) return;
    auto state = snap.assistant.session_state;
    bool between_turns = (state == AssistantState::StreamingUserAudio ||
                          state == AssistantState::WaitingForFollowup) &&
                         !snap.audio.assistant_speaking && !snap.audio.turn_complete_pending &&
                         !awaitingReply();
    if (!between_turns || snap.assistant.ws_state != WsState::CONNECTED) return;
    m_handoff_pending = false;
    {
        std::lock_guard<std::mutex> lock(m_turn_mutex);
        m_force_resume = true;
    }
    LOGI_NET("Handing the session to a new connection.");
    requestRestart(true);
}

void GeminiProtocol::interruptReply() {
    auto snap = sysdb.snapshot();
    if (!snap.audio.assistant_speaking) return;
    bool arriving = !snap.audio.turn_complete_pending;
    BufferManager::getInstance().flush(Buffers::VOICE_RX_BUF);
    sysdb.mutate([](SystemState& s) {
        if (s.audio.assistant_speaking) s.audio.turn_complete_pending = true;
    });
    expectReply();   // to what the person says next; holds the silence timeout
    if (!arriving) {
        LOGI_NET("Interrupting the reply (fully received; flushed).");
        return;
    }
    // The rest of the reply is still queued on the connection, and the
    // connection downloads at about real time (5.7 KB TCP window): whatever
    // Gemini says next would wait behind it (20 s on the device). Start a
    // new connection resuming the conversation instead; the mic audio waits
    // in MIC_TX_BUF until its setup completes.
    LOGI_NET("Interrupting the reply; reconnecting to drop its queued audio.");
    m_interrupt_pending = true;
    {
        std::lock_guard<std::mutex> lock(m_turn_mutex);
        m_force_resume = true;
        m_setup_complete = false;   // the pump holds the mic audio from now
    }
    // With a close frame: dropped abruptly, the old connection still held
    // the session and resuming it failed (close 1011 "Internal error").
    requestRestart(true);
}

bool GeminiProtocol::simulateGoAway() {
    if (!m_accept_audio || sysdb.snapshot().assistant.ws_state != WsState::CONNECTED) return false;
    LOGW_NET("Simulated goAway; handing off at the next turn boundary.");
    m_handoff_pending = true;
    xTaskNotify(getHandle(), NOTIFY_RESTART_BIT, eSetBits);
    return true;
}

void GeminiProtocol::endSession() {
    SessionSettings cfg = m_settings_source ? m_settings_source() : SessionSettings{};
    auto snap = sysdb.snapshot();
    bool quiet;
    {
        std::lock_guard<std::mutex> lock(m_turn_mutex);
        quiet = m_setup_complete && m_pending_turn.empty() && m_reply_wait_us == 0;
    }
    // Closed if a reply is owed or playing: its audio would reach the next
    // session. And after a goAway the connection is about to end anyway.
    if (cfg.keepalive_s == 0 || !quiet || !m_client || m_handoff_pending || m_parked ||
        snap.assistant.ws_state != WsState::CONNECTED || snap.audio.assistant_speaking) {
        closeConnection();
        return;
    }
    m_accept_audio = false;
    m_parked = true;
    esp_timer_stop(m_park_timer);
    esp_timer_start_once(m_park_timer, (uint64_t)cfg.keepalive_s * 1000000);
    LOGI_NET("Keeping the connection open for %d s.", (int)cfg.keepalive_s);
}

void GeminiProtocol::closeConnection() {
    // Check the client, not ws_state: a failed connect reports DISCONNECTED
    // while the client lives on and reconnects every reconnect_timeout_ms,
    // keeping its task and a TLS session (and a Gemini session) while idle.
    if (!m_client && sysdb.snapshot().assistant.ws_state == WsState::DISCONNECTED) {
        return;
    }
    LOGI_NET("Closing WebSocket connection...");
    m_accept_audio = false;
    m_handoff_pending = false;
    m_parked = false;
    esp_timer_stop(m_park_timer);
    m_restart_requested = false;
    m_interrupt_pending = false;
    {
        // The handle stays current for the whole connection (one is sent
        // after setup), so resume_min counts from when the conversation ended.
        std::lock_guard<std::mutex> lock(m_turn_mutex);
        if (!m_resume_handle.empty()) m_resume_handle_us = esp_timer_get_time();
    }
    resetTextTurn();
    {
        std::lock_guard<std::mutex> lock(m_client_mutex);   // vs a fresh-retry restart
        if (m_client) {
            m_client.close(pdMS_TO_TICKS(1000));
            m_client.stop();
            m_client.destroy();
        }
    }
    sysdb.mutate([](SystemState& s) {
        s.assistant.ws_state = WsState::DISCONNECTED;
        s.audio.assistant_speaking = false;
    });
}

void GeminiProtocol::connect() {
    sysdb.mutate([](SystemState& s) {
        s.assistant.connect_requested = true;
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Transmit API
// ─────────────────────────────────────────────────────────────────────────────

void GeminiProtocol::transmitSetupHandshake() {
    if (!m_client.isConnected()) return;

    // Start from the compiled-in setup (model, voice, tool declarations), then
    // apply the saved settings and the long-term memory.
    PsramAllocator psramAlloc;
    JsonDocument doc(&psramAlloc);
    DeserializationError err = deserializeJson(doc, GeminiSkills::SETUP_HANDSHAKE_JSON);
    if (err) {
        LOGE_NET("Failed to parse SETUP_HANDSHAKE_JSON (%s); sending it unmodified", err.c_str());
        m_client.sendLargeText(GeminiSkills::SETUP_HANDSHAKE_JSON, strlen(GeminiSkills::SETUP_HANDSHAKE_JSON),
                               pdMS_TO_TICKS(2000));
        return;
    }
    JsonObject setup = doc["setup"];

    SessionSettings cfg = m_settings_source ? m_settings_source() : SessionSettings{};
    if (!cfg.model.empty()) {
        setup["model"] = cfg.model.rfind("models/", 0) == 0 ? cfg.model : "models/" + cfg.model;
    }
    if (!cfg.voice.empty()) {
        setup["generationConfig"]["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"] = cfg.voice;
    }
    // Input transcription stays on even with transcripts off: it is how the
    // device knows Gemini heard the person and a reply is owed (the silence
    // timeout waits for it). Off, the session ended 3 s after speech while
    // gemini-2.5 was still working (replies ~12 s later with tool calls).
    // The text is then neither shown nor logged.
    m_record_transcripts = cfg.transcripts;
    if (!cfg.transcripts) {
        setup.remove("outputAudioTranscription");
    }
    TranscriptLog::instance().setLogging(cfg.transcripts && cfg.transcript_log);

    bool search = false;
    {
        std::lock_guard<std::mutex> lock(m_turn_mutex);
        m_last_setup_model = setup["model"] | "";
        search = cfg.web_search && modelHasGoogleSearch(m_last_setup_model) &&
                 m_search_refused_model != m_last_setup_model;
        m_search_in_setup = search;
        m_search_quota_closed = false;
    }
    std::string instruction = cfg.system_prompt;
    std::string memory = sd_storage::Fs::readText("/sdcard/gemini_memory.txt");
    if (!memory.empty()) {
        if (!instruction.empty()) instruction += "\n\n";
        instruction += "You have access to the following long-term memory context containing facts, notes, "
                       "or preferences about the user from previous conversations. Use it to inform your responses:\n";
        instruction += memory;
    }
    if (search) {
        // With ~30 function declarations and a long prompt, gemini-2.5 on the
        // device answered "I don't have real-time data" without searching;
        // this line made it search.
        if (!instruction.empty()) instruction += "\n\n";
        instruction += "You have Google Search. Use it for anything current or that you are unsure of: news, "
                       "prices, markets, sports, weather, events, recent facts. Never say you lack real-time "
                       "data without searching first.";
    }
    if (!instruction.empty()) {
        JsonArray parts = setup["systemInstruction"]["parts"].to<JsonArray>();
        parts.add<JsonObject>()["text"] = instruction;
    }

    // Always ask for resumption handles, and resume the last conversation
    // if it is recent. Compression lets Gemini drop old turns instead of
    // ending the session at the context limit (~15 min of audio).
    int resume_age_s = -1;
    {
        std::lock_guard<std::mutex> lock(m_turn_mutex);
        int64_t age_us = esp_timer_get_time() - m_resume_handle_us;
        m_resuming = !m_resume_handle.empty() &&
                     (m_force_resume || (cfg.resume_min > 0 &&
                                         age_us < (int64_t)cfg.resume_min * 60 * 1000000));
        m_force_resume = false;
        JsonObject resumption = setup["sessionResumption"].to<JsonObject>();
        if (m_resuming) {
            resumption["handle"] = m_resume_handle;
            resume_age_s = (int)(age_us / 1000000);
        }
    }
    setup["contextWindowCompression"]["slidingWindow"].to<JsonObject>();

    // Field names checked against gemini-2.5-flash-native-audio-latest and
    // gemini-3.8-live (a bad enum value is refused with 1007).
    JsonObject aad;
    auto detection = [&]() {
        if (aad.isNull()) aad = setup["realtimeInputConfig"]["automaticActivityDetection"].to<JsonObject>();
        return aad;
    };
    if (cfg.vad_start == 1 || cfg.vad_start == 2) {
        detection()["startOfSpeechSensitivity"] = cfg.vad_start == 1 ? "START_SENSITIVITY_LOW" : "START_SENSITIVITY_HIGH";
    }
    if (cfg.vad_end == 1 || cfg.vad_end == 2) {
        detection()["endOfSpeechSensitivity"] = cfg.vad_end == 1 ? "END_SENSITIVITY_LOW" : "END_SENSITIVITY_HIGH";
    }
    if (cfg.vad_prefix_ms) detection()["prefixPaddingMs"] = cfg.vad_prefix_ms;
    if (cfg.vad_silence_ms) detection()["silenceDurationMs"] = cfg.vad_silence_ms;

    if (search) {
        // get_weather is the fallback for models without search; with search
        // it would be one more tool to choose from for the same answer.
        JsonArray decls = setup["tools"][0]["functionDeclarations"];
        for (size_t i = 0; i < decls.size(); ++i) {
            if (strcmp(decls[i]["name"] | "", "get_weather") == 0) {
                decls.remove(i);
                break;
            }
        }
        setup["tools"].add<JsonObject>()["googleSearch"].to<JsonObject>();
    }

    if (m_remote_decls_source) {
        JsonArray decls = setup["tools"][0]["functionDeclarations"];
        m_remote_decls_source(decls, search, m_remote_decls_ctx);
    }

    std::string payload;
    serializeJson(doc, payload);
    LOGI_NET("Uplinking setup: model=%s voice=%s transcripts=%s search=%s vad=%u/%u/%u/%u instruction=%zu bytes (payload %zu bytes) resume=%s",
             setup["model"] | "?",
             setup["generationConfig"]["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"] | "?",
             !cfg.transcripts ? "off" : cfg.transcript_log ? "on+log" : "on",
             search ? "on" : "off", cfg.vad_start, cfg.vad_end, cfg.vad_prefix_ms, cfg.vad_silence_ms,
             instruction.size(), payload.size(),
             resume_age_s < 0 ? "no" : (std::to_string(resume_age_s) + " s old").c_str());
    m_client.sendLargeText(payload.c_str(), payload.length(), pdMS_TO_TICKS(2000));
}

void GeminiProtocol::transmitToolResponse(const char* call_id, const char* json_result) {
    if (!m_client.isConnected() || !call_id) return;
    
    JsonDocument doc;
    JsonObject toolResponse = doc["toolResponse"].to<JsonObject>();
    JsonArray functionResponses = toolResponse["functionResponses"].to<JsonArray>();
    JsonObject funcResp = functionResponses.add<JsonObject>();
    
    funcResp["id"] = call_id;
    
    JsonObject responseObj = funcResp["response"].to<JsonObject>();
    
    if (json_result) {
        JsonDocument resultDoc;
        DeserializationError error = deserializeJson(resultDoc, json_result);
        if (!error) {
            responseObj["output"] = resultDoc.as<JsonVariant>();
        } else {
            responseObj["output"].to<JsonObject>();
        }
    } else {
        responseObj["output"].to<JsonObject>();
    }
    
    std::string payload;
    serializeJson(doc, payload);
    // A lost response leaves the model waiting for it: the reply never comes.
    // Under load another send can hold the client's lock for over a second
    // (a weather answer was dropped while the news answer went out).
    for (int attempt = 1; attempt <= TOOL_SEND_ATTEMPTS; ++attempt) {
        if (m_client.sendText(payload.c_str(), payload.length(), pdMS_TO_TICKS(TOOL_SEND_TIMEOUT_MS)) >= 0) {
            if (attempt > 1) LOGW_NET("Tool response %s sent on attempt %d", call_id, attempt);
            return;
        }
        if (!m_client.isConnected()) break;
    }
    LOGE_NET("Tool response %s could not be sent; the reply will not come", call_id);
}

void GeminiProtocol::transmitAudioUplink(const char* base64_pcm) {
    if (!m_client || !m_client.isConnected() || !base64_pcm) return;
    
    // Half-duplex: no uplink while the assistant speaks, unless barge-in.
    if (!m_barge_in && sysdb.assistantSpeaking()) {
        return;
    }
    
    int payload_len = snprintf(m_static_payload_arena, 4096, 
                               "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}", 
                               base64_pcm);
    if (payload_len > 0 && payload_len < 4096) {
        int ret = m_client.sendText(m_static_payload_arena, payload_len, pdMS_TO_TICKS(2000));
        if (ret < 0) {
            LOGW_NET("Failed to send audio uplink chunk, err=%d", ret);
        }
    }
}

void GeminiProtocol::sendTextDirect(const char* text) {
    if (isConnected() && text) {
        m_client.sendText(text, strlen(text), pdMS_TO_TICKS(1000));
    }
}

void GeminiProtocol::sendTextTurn(const std::string& text) {
    std::lock_guard<std::mutex> lock(m_turn_mutex);
    m_pending_turn = text;
    m_reply_wait_us = esp_timer_get_time();
    if (m_setup_complete && isConnected()) flushTextTurn();
}

void GeminiProtocol::flushTextTurn() {
    if (m_pending_turn.empty()) return;
    JsonDocument doc;
    doc["realtimeInput"]["text"] = m_pending_turn;
    std::string out;
    serializeJson(doc, out);
    LOGI_NET("Sending a text turn (%u chars)", (unsigned)m_pending_turn.size());
    m_client.sendText(out.c_str(), out.size(), pdMS_TO_TICKS(1000));
    m_pending_turn.clear();
}

bool GeminiProtocol::awaitingReply() {
    std::lock_guard<std::mutex> lock(m_turn_mutex);
    return m_reply_wait_us != 0 && esp_timer_get_time() - m_reply_wait_us < REPLY_WAIT_US;
}

void GeminiProtocol::expectReply() {
    std::lock_guard<std::mutex> lock(m_turn_mutex);
    m_reply_wait_us = esp_timer_get_time();
}

void GeminiProtocol::resetTextTurn() {
    std::lock_guard<std::mutex> lock(m_turn_mutex);
    m_setup_complete = false;
    m_pending_turn.clear();
    m_reply_wait_us = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Event Handlers & Frame Parsing
// ─────────────────────────────────────────────────────────────────────────────

void GeminiProtocol::websocketEventHandler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    auto* self = static_cast<GeminiProtocol*>(handler_args);
    auto* data = static_cast<esp_websocket_event_data_t*>(event_data);

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            LOGI_NET("WebSocket established.");
            self->m_accept_audio = true;
            self->m_rx_frames = 0;
            self->m_rx_dropped_frames = 0;
            self->m_rx_audio_bytes = 0;
            self->m_frame_overflowed = false;
            {
                std::lock_guard<std::mutex> lock(self->m_turn_mutex);
                self->m_setup_complete = false;
            }
            self->transmitSetupHandshake();
            sysdb.mutate([](SystemState& s) {
                s.assistant.ws_state = WsState::CONNECTED;
            });
            break;
            
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 0x08) {
                // Close frame: 2-byte code, then the reason (e.g. 1008
                // "Requested entity was not found." for an expired handle).
                const auto* p = reinterpret_cast<const uint8_t*>(data->data_ptr);
                if (data->payload_offset == 0 && data->data_len >= 2) {
                    int reason_len = std::min(data->data_len - 2, 120);
                    LOGW_NET("Gemini closed the connection: %d %.*s", (p[0] << 8) | p[1],
                             reason_len, data->data_ptr + 2);
                    std::string reason(data->data_ptr + 2, reason_len);
                    std::lock_guard<std::mutex> lock(self->m_turn_mutex);
                    if (self->m_search_in_setup && !self->m_setup_complete &&
                        reason.find("quota") != std::string::npos) {
                        self->m_search_quota_closed = true;
                    }
                    // 1011: Gemini dropped the session, and its handle with
                    // it (the next setup resuming it was refused with 1007
                    // "Invalid session handle", costing a reconnect). Not on
                    // the old connection of a restart: the new one resumes.
                    if (((p[0] << 8) | p[1]) == 1011 && !self->m_restarting &&
                        !self->m_resume_handle.empty()) {
                        self->m_resume_handle.clear();
                        LOGW_NET("Session ended by the server; the next one starts fresh.");
                    }
                }
                break;
            }
            if (data->op_code == 1 || data->data_len > 0) {
                if (data->payload_offset == 0) {
                    self->m_assembly_idx = 0;
                    self->m_frame_overflowed = false;
                }

                if (!self->m_frame_overflowed) {
                    if (self->m_assembly_idx + data->data_len < MAX_INCOMING_FRAME_SIZE) {
                        std::memcpy(self->m_assembly_scratch + self->m_assembly_idx, data->data_ptr, data->data_len);
                        self->m_assembly_idx += data->data_len;
                    } else {
                        self->m_frame_overflowed = true;
                        self->m_rx_dropped_frames++;
                        LOGE_NET("Incoming frame oversized for assembly buffer! Expected size: %d, current idx: %u, extra chunk len: %d",
                                 (int)data->payload_len, (unsigned)self->m_assembly_idx, (int)data->data_len);
                    }
                }

                if (data->payload_offset + data->data_len >= data->payload_len) {
                    if (!self->m_frame_overflowed && self->m_assembly_idx < MAX_INCOMING_FRAME_SIZE) {
                        self->m_assembly_scratch[self->m_assembly_idx] = '\0';
                        // Interruptions are small frames; audio frames are big.
                        if (self->m_assembly_idx < 1024 &&
                            strstr(reinterpret_cast<char*>(self->m_assembly_scratch), "\"interrupted\"")) {
                            self->m_interrupt_pending = true;
                        }
                        // Audio frames are big; control and transcript frames are small.
                        if (self->m_assembly_idx >= 1024) {
                            self->noteAudioFrameArrival(self->m_assembly_idx);
                        }
                        const int64_t queue_start_us = esp_timer_get_time();
                        // Long wait on purpose: while blocked, the WS task stops reading the
                        // socket, so the TCP window closes and Gemini pauses sending. Not
                        // while a restart is pending (barge-in): its close frame needs the
                        // client lock this task holds, and the old frames are unwanted.
                        BaseType_t ok = pdFALSE;
                        for (uint32_t waited = 0; !self->m_restart_requested && !self->m_restarting;
                             waited += 100) {
                            ok = xRingbufferSend(self->m_incoming_psram_rb, self->m_assembly_scratch,
                                                 self->m_assembly_idx + 1, pdMS_TO_TICKS(100));
                            if (ok == pdTRUE || waited + 100 >= INCOMING_RB_MAX_BLOCK_MS) break;
                        }
                        const int64_t queued_us = esp_timer_get_time() - queue_start_us;
                        if (queued_us >= 10000) self->m_reply_blocked_us += queued_us;
                        if (ok != pdTRUE && (self->m_restart_requested || self->m_restarting)) {
                            // Discarded: a frame of the connection being replaced.
                        } else if (ok == pdTRUE) {
                            self->m_rx_frames++;
                            if (self->getHandle() != nullptr) {
                                xTaskNotify(self->getHandle(), (1u << 15), eSetBits);
                            }
                        } else {
                            self->m_rx_dropped_frames++;
                            LOGE_NET("PSRAM Ring Buffer Full! Dropped Gemini frame of size %d", (int)self->m_assembly_idx);
                        }
                    } else {
                        if (self->m_frame_overflowed) {
                            LOGE_NET("Frame assembly overflowed, discarding frame of size %d", (int)data->payload_len);
                        } else {
                            self->m_rx_dropped_frames++;
                            LOGE_NET("Frame assembly exceeded max size, dropping frame");
                        }
                    }
                    self->m_frame_overflowed = false; // Reset for next frame
                }
            }
            break;
            
        // A close by the server ends in CLOSED, not DISCONNECTED (the client
        // task stops instead of reconnecting): the same handling, or the
        // session would think it is still connected.
        case WEBSOCKET_EVENT_CLOSED:
        case WEBSOCKET_EVENT_DISCONNECTED:
            LOGW_NET("WebSocket disconnected. Stats: rx_frames=%u, rx_dropped=%u, rx_audio_bytes=%u",
                     (unsigned)self->m_rx_frames, (unsigned)self->m_rx_dropped_frames, (unsigned)self->m_rx_audio_bytes);
            // Note: stop() and destroy() must NEVER be called from within the websocket event handler.
            // AssistantService will safely invoke closeConnection() outside this task context.
            if (self->m_restarting) {
                break;   // the old connection of a restart
            }
            if (self->m_accept_audio) {
                // Closed before setupComplete while resuming: the handle was
                // refused (1008 "not found" once it expires). Forget it and
                // reconnect fresh, keeping the session (and a queued text turn).
                bool refused;
                bool search_refused = false;
                {
                    std::lock_guard<std::mutex> lock(self->m_turn_mutex);
                    if (self->m_search_quota_closed) {
                        search_refused = true;
                        self->m_search_quota_closed = false;
                        self->m_search_refused_model = self->m_last_setup_model;
                    }
                    refused = self->m_resuming && !self->m_setup_complete;
                    if (refused) {
                        self->m_resume_handle.clear();
                        self->m_resuming = false;
                    }
                }
                if (search_refused) {
                    LOGW_NET("Google Search refused (no quota on %s); reconnecting without it.",
                             self->m_search_refused_model.c_str());
                    self->requestRestart(false);
                    break;
                }
                if (refused) {
                    LOGW_NET("Resumption refused; reconnecting without the handle.");
                    self->requestRestart(false);
                    break;
                }
                if (self->m_handoff_pending) {
                    LOGW_NET("Closed after goAway; resuming on a new connection.");
                    self->requestRestart(false);
                    break;
                }
            }
            self->resetTextTurn();
            sysdb.mutate([](SystemState& s) {
                s.assistant.ws_state = WsState::DISCONNECTED;
                s.audio.assistant_speaking = false;
            });
            break;
            
        case WEBSOCKET_EVENT_ERROR:
            LOGE_NET("WebSocket socket error.");
            // Note: stop() and destroy() must NEVER be called from within the websocket event handler.
            sysdb.mutate([](SystemState& s) {
                s.assistant.ws_state = WsState::ERROR_STATE;
                s.audio.assistant_speaking = false;
            });
            break;
    }
}

void GeminiProtocol::noteAudioFrameArrival(size_t bytes) {
    const int64_t now_us = esp_timer_get_time();
    const int64_t last_us = m_reply_last_us.exchange(now_us);
    if (m_reply_first_us.load() == 0) {
        m_reply_first_us = now_us;
    } else if (last_us != 0 && now_us - last_us > m_reply_max_gap_us.load()) {
        m_reply_max_gap_us = now_us - last_us;
    }
    m_reply_frame_bytes += bytes;
    m_reply_frames++;
}

void GeminiProtocol::logReplyDelivery(const char* how) {
    const int64_t first_us = m_reply_first_us.exchange(0);
    const int64_t last_us = m_reply_last_us.exchange(0);
    const int64_t max_gap_us = m_reply_max_gap_us.exchange(0);
    const int64_t blocked_us = m_reply_blocked_us.exchange(0);
    const uint32_t frame_bytes = m_reply_frame_bytes.exchange(0);
    const uint32_t frames = m_reply_frames.exchange(0);
    if (frames == 0) return;
    // Base64 is 4 chars per 3 bytes; 24 kHz 16-bit mono is 48 bytes per ms.
    // The JSON around the data makes this a slight overestimate.
    const uint32_t audio_ms = frame_bytes / 4 * 3 / 48;
    const uint32_t span_ms = (uint32_t)((last_us - first_us) / 1000);
    LOGI_NET("Reply delivery (%s): %u frames, ~%u ms audio over %u ms (%u.%02ux real time), "
             "largest gap %u ms, queue blocked %u ms",
             how, (unsigned)frames, (unsigned)audio_ms, (unsigned)span_ms,
             span_ms ? (unsigned)(audio_ms / span_ms) : 0u,
             span_ms ? (unsigned)(audio_ms * 100 / span_ms % 100) : 0u,
             (unsigned)(max_gap_us / 1000), (unsigned)(blocked_us / 1000));
}

void GeminiProtocol::processIncomingFrame(char* payload, size_t length) {
    char old_char = payload[length];
    payload[length] = '\0';

    const char* data_key = "\"data\": \"";
    char* data_start = strstr(payload, data_key);
    if (!data_start) {
        data_key = "\"data\":\"";
        data_start = strstr(payload, data_key);
    }

    if (data_start) {
        data_start += strlen(data_key);
        char* data_end = strchr(data_start, '"');
        if (data_end) {
            *data_end = '\0';

            if (!m_accept_audio || m_interrupt_pending) {
                // The session was closed while this frame sat in the queue,
                // or the person interrupted the reply it belongs to.
                *data_end = '"';
                payload[length] = old_char;
                return;
            }
            {
                std::lock_guard<std::mutex> lock(m_turn_mutex);
                m_reply_wait_us = 0;   // the reply has started
            }
            // If transitioning to speaking, flush stale voice data and update sysdb (notifies reactors once)
            if (!sysdb.assistantSpeaking()) {
                BufferManager::getInstance().flush(Buffers::VOICE_RX_BUF);
                sysdb.mutate([](SystemState& s) {
                    s.assistant.session_state = AssistantState::AssistantSpeaking;
                    s.assistant.visual_state  = AssistantVisualState::Speaking;
                    s.audio.assistant_speaking = true;
                });
                AudioOrchestrator::getInstance().notifyVoiceStarted();
            }

            size_t b64_len = data_end - data_start;
            size_t written = 0;

            // Single-pass base64 decode using the maximum static PCM arena size limit
            int decode_res = mbedtls_base64_decode(m_static_pcm_scratch_arena, STATIC_PCM_ARENA_MAX_SIZE, &written,
                                                   reinterpret_cast<const unsigned char*>(data_start), b64_len);
            if (decode_res == 0) {
                if (written > 0) {
                    m_rx_audio_bytes += written;

                    // Gemini streams faster than real time. Rather than dropping when the
                    // playback buffer is full, block until the speaker drains room. This
                    // backs up m_incoming_psram_rb, which in turn stalls the WS event
                    // handler and applies TCP backpressure to the server.
                    // Bail out if speaking ends (session idle / disconnect) or playback stalls.
                    auto& bm = BufferManager::getInstance();
                    bool sent = false;
                    for (uint32_t waited_ms = 0; waited_ms < VOICE_RX_MAX_BLOCK_MS; waited_ms += 100) {
                        if (bm.send(Buffers::VOICE_RX_BUF, m_static_pcm_scratch_arena, written, pdMS_TO_TICKS(100))) {
                            sent = true;
                            break;
                        }
                        if (!m_running || !sysdb.assistantSpeaking() || m_interrupt_pending) break;
                    }
                    if (!sent) {
                        m_rx_dropped_frames++;
                        LOGW_NET("Audio drop: VOICE_RX_BUF is full (playback not draining)!");
                    }
                }
            } else {
                LOGE_NET("Base64 decode failed! err=-0x%04X, b64_len=%d", -decode_res, (int)b64_len);
            }
            *data_end = '"';
        }
        // Transcriptions usually come in their own frames, but can share one with audio.
        if (strstr(payload, "Transcription\"")) {
            JsonDocument filter;
            filter["serverContent"]["inputTranscription"]["text"] = true;
            filter["serverContent"]["outputTranscription"]["text"] = true;
            JsonDocument doc;
            if (!deserializeJson(doc, static_cast<const char*>(payload), length,
                                 DeserializationOption::Filter(filter))) {
                recordTranscription(doc["serverContent"]);
            }
        }
    } else {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload);
        if (!err) {
            if (!doc["setupComplete"].isNull()) {
                std::lock_guard<std::mutex> lock(m_turn_mutex);
                m_setup_complete = true;
                LOGI_NET("Setup complete%s", m_resuming ? " (conversation resumed)" : "");
                flushTextTurn();
            }
            JsonObjectConst resumption = doc["sessionResumptionUpdate"];
            const char* handle = resumption["newHandle"] | "";
            if ((resumption["resumable"] | false) && handle[0]) {
                std::lock_guard<std::mutex> lock(m_turn_mutex);
                m_resume_handle = handle;
                m_resume_handle_us = esp_timer_get_time();
            }
            bool turn_complete = doc["serverContent"]["turnComplete"].as<bool>() || doc["turnComplete"].as<bool>();
            recordTranscription(doc["serverContent"]);
            if (turn_complete) {
                LOGI_NET("Assistant turn complete");
                logReplyDelivery("complete");
                m_turns_completed++;
                {
                    std::lock_guard<std::mutex> lock(m_turn_mutex);
                    m_reply_wait_us = 0;
                    m_resume_handle_us = esp_timer_get_time();   // last activity
                }
                m_interrupt_pending = false;   // the interrupted reply has ended
                TranscriptLog::instance().closeTurn();
                sysdb.mutate([](SystemState& s) {
                    s.audio.turn_complete_pending = true;
                });
            }

            if (doc["serverContent"]["interrupted"] | false) {
                // Barge-in: Gemini heard the person over its reply. Drop what
                // is queued; the drain after turn_complete_pending ends the
                // voice (music, mic and state follow as after a reply).
                LOGI_NET("Reply interrupted by the user.");
                logReplyDelivery("interrupted");
                BufferManager::getInstance().flush(Buffers::VOICE_RX_BUF);
                m_interrupt_pending = false;
                sysdb.mutate([](SystemState& s) {
                    if (s.audio.assistant_speaking) s.audio.turn_complete_pending = true;
                });
            }

            if (!doc["error"].isNull()) {
                LOGE_NET("Gemini API server error in frame!");
                sysdb.mutate([](SystemState& s) {
                    s.assistant.session_state = AssistantState::ErrorCooldown;
                });
            }

            if (!doc["goAway"].isNull()) {
                LOGW_NET("goAway (time left %s); handing off at the next turn boundary.",
                         doc["goAway"]["timeLeft"] | "?");
                m_handoff_pending = true;
                maybeHandOff(sysdb.snapshot());
            }

            JsonObjectConst toolCall = doc["toolCall"];
            if (!toolCall.isNull()) {
                handleToolCall(toolCall);
            }
        }
    }

    payload[length] = old_char;
}

void GeminiProtocol::recordTranscription(JsonObjectConst serverContent) {
    if (serverContent.isNull()) return;
    const char* in = serverContent["inputTranscription"]["text"] | "";
    const char* out = serverContent["outputTranscription"]["text"] | "";
    if (in[0]) {
        // Gemini heard the person, so a reply follows (possibly seconds later).
        expectReply();
        if (m_record_transcripts) TranscriptLog::instance().append(TranscriptLog::Role::User, in, strlen(in));
    }
    if (out[0] && m_record_transcripts) TranscriptLog::instance().append(TranscriptLog::Role::Model, out, strlen(out));
}

void GeminiProtocol::handleToolCall(JsonObjectConst toolCall) {
    JsonArrayConst functionCalls = toolCall["functionCalls"];
    if (functionCalls.isNull()) return;

    for (JsonObjectConst funcCall : functionCalls) {
        if (funcCall.isNull()) continue;

        const char* name = funcCall["name"] | "";
        const char* id = funcCall["id"];
        // Parameterless calls may arrive without an "args" object.
        JsonObjectConst argsObj = funcCall["args"];
        if (!id) {
            LOGW_NET("Tool call '%s' without an id; cannot reply", name);
            continue;
        }

        // Every call must get a toolResponse, or the model waits on it.
        auto& slot = m_static_skill_event_slot;
        slot.reset();
        if (!GeminiSkills::decode_incoming_arguments(name, argsObj, slot)) {
            if (m_remote_tool_handler && m_remote_tool_handler(id, name, argsObj, m_remote_tool_ctx)) {
                LOGI_NET("Remote MCP tool dispatched: %s", name);
                expectReply();
                slot.reset();
                continue;
            }
            LOGW_NET("Rejected tool call '%s': %s", name, slot.error);
            JsonDocument err;
            err["status"] = "error";
            err["message"] = slot.error;
            std::string out;
            serializeJson(err, out);
            transmitToolResponse(id, out.c_str());
            slot.reset();
            continue;
        }
        std::strncpy(slot.call_id, id, sizeof(slot.call_id) - 1);

        LOGI_NET("Tool request: %s", name);
        expectReply();   // the model answers after our toolResponse
        if (m_tool_handler) {
            m_tool_handler(slot, m_tool_ctx);
        } else {
            transmitToolResponse(id, "{\"status\":\"error\",\"message\":\"No tool handler\"}");
        }
        slot.reset();
    }
}

void GeminiProtocol::run() {
    LOGI_NET("Gemini Protocol background PSRAM parsing task actively running.");
    
    static constexpr uint32_t NOTIFY_RINGBUF_BIT = (1u << 15);

    while (m_running) {
        uint32_t changed_bits = 0;
        // Block until we get a notification (either database changes or new ring buffer items)
        BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &changed_bits, portMAX_DELAY);
        if (!m_running) break;

        if (notified == pdTRUE) {
            // 1. Process all available items in the ring buffer
            if (changed_bits & NOTIFY_RINGBUF_BIT) {
                size_t frame_size = 0;
                char* frame_data;
                while ((frame_data = static_cast<char*>(xRingbufferReceive(m_incoming_psram_rb, &frame_size, 0))) != nullptr) {
                    if (frame_size > 1) {
                        processIncomingFrame(frame_data, frame_size - 1);
                    }
                    vRingbufferReturnItem(m_incoming_psram_rb, frame_data);
                }
            }

            if ((changed_bits & NOTIFY_PARK_EXPIRED_BIT) && m_parked) {
                LOGI_NET("Keep-alive window over.");
                closeConnection();
            }
            if (changed_bits & NOTIFY_RESTART_BIT) {
                if (m_restart_requested.exchange(false)) {
                    restartConnection();
                } else {
                    maybeHandOff(sysdb.snapshot());
                }
            }

            // 2. Process database state changes
            uint32_t db_changed = changed_bits & ~(NOTIFY_RINGBUF_BIT | NOTIFY_RESTART_BIT | NOTIFY_PARK_EXPIRED_BIT);
            if (db_changed > 0) {
                m_last_changed = db_changed;
                SystemState snap = EmbeddedSysDb::getInstance().snapshot();
                onStateChanged(m_last_changed, snap);
            }
        }
    }
}
