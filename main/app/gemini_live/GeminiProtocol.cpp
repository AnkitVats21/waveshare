#include "GeminiProtocol.h"
#include "GeminiAudioPump.h"
#include "gemini_skills_generated.h"
#include "common/AppLogger.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include "sdkconfig.h"
#include "esp_timer.h"

#include "mbedtls/base64.h"
#include "services/BufferManager.h"
#include "app/audio/MicCapture.h"
#include "app/audio/SpeakerPlayback.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include <string>
#include <cstring>
#include <vector>

static const char* const GEMINI_LIVE_BASE_URL = "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=";
static constexpr size_t STATIC_PCM_ARENA_MAX_SIZE = 24576; // 24KB ceiling

// ─────────────────────────────────────────────────────────────────────────────
// Construction & Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

GeminiProtocol::GeminiProtocol()
    : ReactorTask({
          "gemini_proto",
          ThreadConfig::StackSize::STACK_GEMINI,
          ThreadConfig::Priority::GEMINI_PROTOCOL,
          ThreadConfig::CORE_NETWORK,
          COMP::ASSISTANT
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
}

GeminiProtocol& GeminiProtocol::getInstance() {
    static GeminiProtocol instance;
    return instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReactorTask Interface
// ─────────────────────────────────────────────────────────────────────────────

void GeminiProtocol::onStateChanged(ComponentMask changed, const SystemState& snap) {
    // Handled directly in run loop via xTaskNotifyWait
}

// ─────────────────────────────────────────────────────────────────────────────
// Background Task Connection Loop
// ─────────────────────────────────────────────────────────────────────────────

void GeminiProtocol::run() {
    LOGI_NET("GeminiProtocol active on Core %d", xPortGetCoreID());

    while (m_running) {
        uint32_t changed_bits = 0;
        // Wait for a state change notification or 100ms tick
        BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &changed_bits, pdMS_TO_TICKS(100));
        if (!m_running) break;

        if (notified == pdTRUE && changed_bits > 0) {
            m_last_changed = changed_bits;
            SystemState snap = EmbeddedSysDb::getInstance().snapshot();
            onStateChanged(m_last_changed, snap);
        }

        auto snap = EmbeddedSysDb::getInstance().snapshot();
        bool requested = snap.assistant.connect_requested;
        bool wifi_ok = snap.system.wifi_connected;
        auto ws = snap.assistant.ws_state;

        if (requested && wifi_ok && (ws == WsState::DISCONNECTED || ws == WsState::ERROR_STATE)) {
            if (ensureClientInitialized()) {
                startClientConnection();
            } else {
                LOGE_NET("WebSocket client initialization failed.");
                EmbeddedSysDb::getInstance().mutate(COMP::ASSISTANT, [](SystemState& s) {
                    s.assistant.ws_state = WsState::ERROR_STATE;
                    s.assistant.connect_requested = false;
                });
            }
        } else if ((!requested || !wifi_ok) && (ws == WsState::CONNECTED || ws == WsState::CONNECTING)) {
            closeConnection();
        }
    }

    m_client.destroy();
}

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket Client Management
// ─────────────────────────────────────────────────────────────────────────────

bool GeminiProtocol::ensureClientInitialized() {
    std::lock_guard<std::mutex> lock(m_client_mutex);
    if (m_client) {
        return true;
    }

    m_ws_uri = GEMINI_LIVE_BASE_URL;
#ifdef CONFIG_GEMINI_API_KEY
    if (std::strlen(CONFIG_GEMINI_API_KEY) == 0) {
        LOGE_NET("CONFIG_GEMINI_API_KEY is empty.");
        return false;
    }
    m_ws_uri += CONFIG_GEMINI_API_KEY;
#else
    LOGE_NET("CONFIG_GEMINI_API_KEY is missing.");
    return false;
#endif

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = m_ws_uri.c_str();
    ws_cfg.buffer_size = 4096;
    ws_cfg.reconnect_timeout_ms = 10000;
    ws_cfg.network_timeout_ms = 10000;
    ws_cfg.task_stack = 10240;
    ws_cfg.task_prio = ThreadConfig::Priority::GEMINI_PROTOCOL;
    ws_cfg.task_core_id = ThreadConfig::CORE_NETWORK;
    ws_cfg.task_core_id_set = true;
    ws_cfg.cert_pem = nullptr;
    ws_cfg.crt_bundle_attach = nullptr;
    ws_cfg.skip_cert_common_name_check = true;

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
    
    EmbeddedSysDb::getInstance().mutate(COMP::ASSISTANT, [](SystemState& s) {
        s.assistant.ws_state = WsState::CONNECTING;
    });

    m_client.stop();
    esp_err_t err = m_client.start();
    if (err != ESP_OK) {
        LOGE_NET("esp_websocket_client_start failed: 0x%x", (unsigned)err);
        EmbeddedSysDb::getInstance().mutate(COMP::ASSISTANT, [](SystemState& s) {
            s.assistant.ws_state = WsState::ERROR_STATE;
            s.assistant.connect_requested = false;
        });
        return false;
    }

    return true;
}

void GeminiProtocol::closeConnection() {
    LOGI_NET("Closing WebSocket connection...");
    if (m_client) {
        m_client.close(pdMS_TO_TICKS(1000));
        m_client.stop();
    }
    EmbeddedSysDb::getInstance().mutate(COMP::ASSISTANT | COMP::AUDIO, [](SystemState& s) {
        s.assistant.ws_state = WsState::DISCONNECTED;
        s.audio.assistant_speaking = false;
    });
}

void GeminiProtocol::connect() {
    EmbeddedSysDb::getInstance().mutate(COMP::ASSISTANT, [](SystemState& s) {
        s.assistant.connect_requested = true;
    });
    xTaskNotifyGive(m_task_handle);
}

// ─────────────────────────────────────────────────────────────────────────────
// Transmit API
// ─────────────────────────────────────────────────────────────────────────────

void GeminiProtocol::transmitSetupHandshake() {
    if (!m_client.isConnected()) return;
    
    LOGI_NET("Uplinking JSON schema handshake compilation payload...");
    m_client.sendText(GeminiSkills::SETUP_HANDSHAKE_JSON, 
                      strlen(GeminiSkills::SETUP_HANDSHAKE_JSON), 
                      pdMS_TO_TICKS(1000));
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
    if (EmbeddedSysDb::getInstance().assistantSpeaking()) {
        return;
    }
    
    int payload_len = snprintf(m_static_payload_arena, 4096, 
                               "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}", 
                               base64_pcm);
    if (payload_len > 0 && payload_len < 4096) {
        m_client.sendText(m_static_payload_arena, payload_len, pdMS_TO_TICKS(2000));
    }
}

void GeminiProtocol::sendTextDirect(const char* text) {
    if (isConnected() && text) {
        m_client.sendText(text, strlen(text), pdMS_TO_TICKS(1000));
    }
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
            self->transmitSetupHandshake();
            EmbeddedSysDb::getInstance().mutate(COMP::ASSISTANT, [](SystemState& s) {
                s.assistant.ws_state = WsState::CONNECTED;
            });
            break;
            
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 1 || data->data_len > 0) {
                if (data->payload_offset == 0) {
                    self->m_incoming_assembly_buffer.clear();
                }

                size_t current_size = self->m_incoming_assembly_buffer.size();
                self->m_incoming_assembly_buffer.resize(current_size + data->data_len);
                std::memcpy(self->m_incoming_assembly_buffer.data() + current_size, data->data_ptr, data->data_len);

                if (data->payload_offset + data->data_len >= data->payload_len) {
                    self->m_incoming_assembly_buffer.push_back('\0');
                    self->processIncomingFrame(self->m_incoming_assembly_buffer.data(), self->m_incoming_assembly_buffer.size() - 1);
                }
            }
            break;
            
        case WEBSOCKET_EVENT_DISCONNECTED:
            LOGW_NET("WebSocket disconnected.");
            EmbeddedSysDb::getInstance().mutate(COMP::ASSISTANT, [](SystemState& s) {
                s.assistant.ws_state = WsState::DISCONNECTED;
            });
            break;
            
        case WEBSOCKET_EVENT_ERROR:
            LOGE_NET("WebSocket socket error.");
            EmbeddedSysDb::getInstance().mutate(COMP::ASSISTANT, [](SystemState& s) {
                s.assistant.ws_state = WsState::ERROR_STATE;
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
            size_t b64_len = data_end - data_start;
            size_t pcm_out_len = 0;

            mbedtls_base64_decode(nullptr, 0, &pcm_out_len, reinterpret_cast<const unsigned char*>(data_start), b64_len);
            
            if (pcm_out_len > 0 && pcm_out_len <= STATIC_PCM_ARENA_MAX_SIZE) {
                size_t written = 0;
                if (mbedtls_base64_decode(m_static_pcm_scratch_arena, pcm_out_len, &written, reinterpret_cast<const unsigned char*>(data_start), b64_len) == 0) {
                    if (written > 0) {
                        // If transitioning to speaking, flush stale 16kHz data and update DB (notifies reactors once)
                        if (!EmbeddedSysDb::getInstance().assistantSpeaking()) {
                            BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);
                            EmbeddedSysDb::getInstance().mutate(COMP::ASSISTANT | COMP::AUDIO, [](SystemState& s) {
                                s.assistant.session_state = AssistantState::AssistantSpeaking;
                                s.assistant.visual_state  = AssistantVisualState::Speaking;
                                s.audio.assistant_speaking = true;
                            });
                        }

                        BufferManager::getInstance().send(Buffers::SPK_RX_BUF, m_static_pcm_scratch_arena, written, pdMS_TO_TICKS(20));
                    }
                }
            }
            *data_end = '"';
        }
    } else {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload);
        if (!err) {
            bool turn_complete = strstr(payload, "\"turnComplete\": true") || strstr(payload, "\"turnComplete\":true") || !doc["turnComplete"].isNull();
            if (turn_complete) {
                LOGI_NET("Assistant turn complete");
                EmbeddedSysDb::getInstance().mutate(COMP::AUDIO, [](SystemState& s) {
                    s.audio.turn_complete_pending = true;
                });
            }

            if (!doc["error"].isNull() || strstr(payload, "\"error\"")) {
                LOGE_NET("Gemini API server error in frame!");
                EmbeddedSysDb::getInstance().mutate(COMP::ASSISTANT, [](SystemState& s) {
                    s.assistant.session_state = AssistantState::ErrorCooldown;
                });
            }

            if (!doc["goAway"].isNull() || strstr(payload, "\"goAway\"")) {
                LOGW_NET("Gemini Live Engine: Received 'goAway' signal.");
                EmbeddedSysDb::getInstance().mutate(COMP::ASSISTANT, [](SystemState& s) {
                    s.assistant.ws_state = WsState::GOING_AWAY;
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

void GeminiProtocol::handleToolCall(JsonObjectConst toolCall) {
    JsonArrayConst functionCalls = toolCall["functionCalls"];
    if (functionCalls.isNull() || functionCalls.size() == 0) return;

    JsonObjectConst funcCall = functionCalls[0];
    if (funcCall.isNull()) return;

    const char* name = funcCall["name"];
    const char* id = funcCall["id"];
    JsonObjectConst argsObj = funcCall["args"];
    
    if (name && id && !argsObj.isNull()) {
        std::memset(&m_static_skill_event_slot, 0, sizeof(m_static_skill_event_slot));
        std::strncpy(m_static_skill_event_slot.call_id, id, sizeof(m_static_skill_event_slot.call_id) - 1);
        
        if (GeminiSkills::decode_incoming_arguments(name, argsObj, m_static_skill_event_slot)) {
            LOGI_NET("Tool request: %s", name);
            if (m_tool_handler) {
                m_tool_handler(m_static_skill_event_slot, m_tool_ctx);
            }
        }
    }
}
