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
#include "sdkconfig.h"
#include "esp_timer.h"

#include "mbedtls/base64.h"
#include "services/BufferManager.h"
#include "app/audio/MicCapture.h"
#include "app/audio/SpeakerPlayback.h"
#include "app/audio/AudioOrchestrator.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include <string>
#include <cstring>
#include <vector>

static const char* const GEMINI_LIVE_BASE_URL = "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=";
static constexpr size_t STATIC_PCM_ARENA_MAX_SIZE = 65536; // 64KB ceiling

static auto& sysdb = EmbeddedSysDb::getInstance();

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
    bool requested = snap.assistant.connect_requested;
    bool wifi_ok = snap.system.wifi_connected;
    auto ws = snap.assistant.ws_state;

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
    ws_cfg.task_stack = 6144;
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

void GeminiProtocol::closeConnection() {
    // Check the client, not ws_state: a failed connect reports DISCONNECTED
    // while the client lives on and reconnects every reconnect_timeout_ms,
    // keeping its task and a TLS session (and a Gemini session) while idle.
    if (!m_client && sysdb.snapshot().assistant.ws_state == WsState::DISCONNECTED) {
        return;
    }
    LOGI_NET("Closing WebSocket connection...");
    m_accept_audio = false;
    resetTextTurn();
    if (m_client) {
        m_client.close(pdMS_TO_TICKS(1000));
        m_client.stop();
        m_client.destroy();
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
    JsonDocument doc;
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
    if (!cfg.transcripts) {
        setup.remove("inputAudioTranscription");
        setup.remove("outputAudioTranscription");
    }
    TranscriptLog::instance().setLogging(cfg.transcripts && cfg.transcript_log);

    std::string instruction = cfg.system_prompt;
    std::string memory = sd_storage::Fs::readText("/sdcard/gemini_memory.txt");
    if (!memory.empty()) {
        if (!instruction.empty()) instruction += "\n\n";
        instruction += "You have access to the following long-term memory context containing facts, notes, "
                       "or preferences about the user from previous conversations. Use it to inform your responses:\n";
        instruction += memory;
    }
    if (!instruction.empty()) {
        JsonArray parts = setup["systemInstruction"]["parts"].to<JsonArray>();
        parts.add<JsonObject>()["text"] = instruction;
    }

    std::string payload;
    serializeJson(doc, payload);
    LOGI_NET("Uplinking setup: model=%s voice=%s transcripts=%s instruction=%zu bytes (payload %zu bytes)",
             setup["model"] | "?",
             setup["generationConfig"]["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"] | "?",
             !cfg.transcripts ? "off" : cfg.transcript_log ? "on+log" : "on",
             instruction.size(), payload.size());
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
    m_client.sendText(payload.c_str(), payload.length(), pdMS_TO_TICKS(1000));
}

void GeminiProtocol::transmitAudioUplink(const char* base64_pcm) {
    if (!m_client || !m_client.isConnected() || !base64_pcm) return;
    
    // Suppress uplink if assistant is speaking
    if (sysdb.assistantSpeaking()) {
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
    m_text_turn_us = esp_timer_get_time();
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

bool GeminiProtocol::awaitingTextReply() {
    std::lock_guard<std::mutex> lock(m_turn_mutex);
    return m_text_turn_us != 0 && esp_timer_get_time() - m_text_turn_us < TEXT_REPLY_WAIT_US;
}

void GeminiProtocol::resetTextTurn() {
    std::lock_guard<std::mutex> lock(m_turn_mutex);
    m_setup_complete = false;
    m_pending_turn.clear();
    m_text_turn_us = 0;
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
                        // Long wait on purpose: while blocked, the WS task stops reading the
                        // socket, so the TCP window closes and Gemini pauses sending.
                        BaseType_t ok = xRingbufferSend(self->m_incoming_psram_rb,
                                                        self->m_assembly_scratch,
                                                        self->m_assembly_idx + 1,
                                                        pdMS_TO_TICKS(INCOMING_RB_MAX_BLOCK_MS));
                        if (ok == pdTRUE) {
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
            
        case WEBSOCKET_EVENT_DISCONNECTED:
            LOGW_NET("WebSocket disconnected. Stats: rx_frames=%u, rx_dropped=%u, rx_audio_bytes=%u",
                     (unsigned)self->m_rx_frames, (unsigned)self->m_rx_dropped_frames, (unsigned)self->m_rx_audio_bytes);
            // Note: stop() and destroy() must NEVER be called from within the websocket event handler.
            // AssistantService will safely invoke closeConnection() outside this task context.
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

            {
                std::lock_guard<std::mutex> lock(m_turn_mutex);
                m_text_turn_us = 0;   // the reply has started
            }
            // If transitioning to speaking, flush stale voice data and update sysdb (notifies reactors once)
            if (!m_accept_audio) {
                // The session was closed while this frame sat in the queue.
                *data_end = '"';
                payload[length] = old_char;
                return;
            }
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
                        if (!m_running || !sysdb.assistantSpeaking()) break;
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
                flushTextTurn();
            }
            bool turn_complete = doc["serverContent"]["turnComplete"].as<bool>() || doc["turnComplete"].as<bool>();
            recordTranscription(doc["serverContent"]);
            if (turn_complete) {
                LOGI_NET("Assistant turn complete");
                TranscriptLog::instance().closeTurn();
                sysdb.mutate([](SystemState& s) {
                    s.audio.turn_complete_pending = true;
                });
            }

            if (!doc["error"].isNull()) {
                LOGE_NET("Gemini API server error in frame!");
                sysdb.mutate([](SystemState& s) {
                    s.assistant.session_state = AssistantState::ErrorCooldown;
                });
            }

            if (!doc["goAway"].isNull()) {
                LOGW_NET("Gemini Live Engine: Received 'goAway' signal.");
                sysdb.mutate([](SystemState& s) {
                    s.assistant.ws_state = WsState::GOING_AWAY;
                    s.audio.assistant_speaking = false;
                });
                if (m_client) m_client.close(pdMS_TO_TICKS(1500));
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
    if (in[0]) TranscriptLog::instance().append(TranscriptLog::Role::User, in, strlen(in));
    if (out[0]) TranscriptLog::instance().append(TranscriptLog::Role::Model, out, strlen(out));
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

            // 2. Process database state changes
            uint32_t db_changed = changed_bits & ~NOTIFY_RINGBUF_BIT;
            if (db_changed > 0) {
                m_last_changed = db_changed;
                SystemState snap = EmbeddedSysDb::getInstance().snapshot();
                onStateChanged(m_last_changed, snap);
            }
        }
    }
}
