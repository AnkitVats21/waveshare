#include "GeminiProtocolTask.h"
#include "GeminiAudioPumpTask.h"
#include "gemini_skills_generated.h"
#include "common/AppLogger.h"
#include "app/event/EventBus.h"
#include "common/events/app_events.h"
#include "app/assistant/AssistantEvents.h"
#include "sdkconfig.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "services/BufferManager.h"
#include "app/audio/MicCapture.h"
#include "app/audio/SpeakerPlayback.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "app/audio/AudioService.h"
#include "app/wake_word/WakeWordDetector.h"
#include <string>
#include <cstring>
#include <vector>

// Component Constants Configuration
static const char* const TAG_GEMINI_WS = "GeminiProto";
static const char* const GEMINI_LIVE_BASE_URL = "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=";
static constexpr size_t WS_NETWORK_BUFFER_SIZE = 32768; // 32KB prevents fragmentation panics and comfortably handles large base64 JSON payload packets

// Preprocessor switch to toggle between testing (Direct-Trust / skip certificate verification) 
// and production-grade secure TLS verification.
// #define USE_PRODUCTION_SECURE_TLS

#ifdef USE_PRODUCTION_SECURE_TLS
// Embedded GlobalSign Root CA - R3 (Trusted Root for Google APIs, cross-signed GTS CA verification)
static const char* const GLOBAL_SIGN_R3_CA_PEM = R"EOF(-----BEGIN CERTIFICATE-----
MIIDXzCCAkegAwIBAgILBAAAAAABIVhTCKIwDQYJKoZIhvcNAQELBQAwTDEgMB4G
A1UECxMXR2xvYmFsU2lnbiBSb290IENBIC0gUjMxEzARBgNVBAoTCkdsb2JhbFNp
Z24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMDkwMzE4MTAwMDAwWhcNMjkwMzE4
MTAwMDAwWjBMMSAwHgYDVQQLExdHbG9iYWxTaWduIFJvb3QgQ0EgLSBSMzETMBEG
A1UEChMKR2xvYmFsU2lnbjETMBEGA1UEAxMKR2xvYmFsU2lnbjCCASIwDQYJKoZI
hvcNAQEBBQADggEPADCCAQoCggEBAMwldpB5BngiFvXAg7aEyiie/QV2EcWtiHL8
RgJDx7KKnQRfJMsuS+FggkbhUqsMgUdwbN1k0ev1LKMPgj0MK66X17YUhhB5uzsT
gHeMCOFJ0mpiLx9e+pZo34knlTifBtc+ycsmWQ1z3rDI6SYOgxXG71uL0gRgykmm
KPZpO/bLyCiR5Z2KYVc3rHQU3HTgOu5yLy6c+9C7v/U9AOEGM+iCK65TpjoWc4zd
QQ4gOsC0p6Hpsk+QLjJg6VfLuQSSaGjlOCZgdbKfd/+RFO+uIEn8rUAVSNECMWEZ
XriX7613t2Saer9fwRPvm2L7DWzgVGkWqQPabumDk3F2xmmFghcCAwEAAaNCMEAw
DgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0OBBYEFI/wS3+o
LkUkrk1Q+mOai97i3Ru8MA0GCSqGSIb3DQEBCwUAA4IBAQBLQNvAUKr+yAzv95ZU
RUm7lgAJQayzE4aGKAczymvmdLm6AC2upArT9fHxD4q/c2dKg8dEe3jgr25sbwMp
jjM5RcOO5LlXbKr8EpbsU8Yt5CRsuZRj+9xTaGdWPoO4zzUhw8lo/s7awlOqzJCK
6fBdRoyV3XpYKBovHd7NADdBj+1EbddTKJd+82cEHhXXipa0095MJ6RMG3NzdvQX
mcIfeg7jLQitChws/zyrVQ4PkX4268NXSb7hLi18YIvDQVETI53O9zJrlAGomecs
Mx86OyXShkDOOyyGeMlhLxS67ttVb9+E7gUJTb0o2HLO02JQZR7rkpeDMdmztcpH
WD9f
-----END CERTIFICATE-----)EOF";
#endif

static std::atomic<bool> g_assistant_currently_talking{false};

// Custom PSRAM Allocators for cJSON Engine
static void* cjson_psram_malloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void cjson_psram_free(void* ptr) {
    heap_caps_free(ptr);
}

GeminiProtocolTask::GeminiProtocolTask(const Config& cfg) : TaskBase(cfg) {
    // Force cJSON to utilize your 8MB OPI PSRAM line natively
    cJSON_Hooks hooks;
    hooks.malloc_fn = cjson_psram_malloc;
    hooks.free_fn = cjson_psram_free;
    cJSON_InitHooks(&hooks);

    // Boot-time allocation of external PSRAM binary scratch arena
    m_static_pcm_scratch_arena = static_cast<uint8_t*>(
        heap_caps_malloc(STATIC_PCM_ARENA_MAX_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    assert(m_static_pcm_scratch_arena != nullptr);

    m_static_pcm_downsampled_arena = static_cast<uint8_t*>(
        heap_caps_malloc(STATIC_PCM_ARENA_MAX_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    assert(m_static_pcm_downsampled_arena != nullptr);

    m_static_payload_arena = static_cast<char*>(
        heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    assert(m_static_payload_arena != nullptr);
}

GeminiProtocolTask& GeminiProtocolTask::getInstance() {
    static Config default_config = {
        .name = "GeminiProtocolTask",
        .stack_size = 12288, 
        .priority = 4,
        .core_id = 0 // Bound to Core 0 for low-level networking loops
    };
    static GeminiProtocolTask instance(default_config);
    return instance;
}

void GeminiProtocolTask::websocketEventHandler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    auto* self = static_cast<GeminiProtocolTask*>(handler_args);
    ConnectionState previous_state = self->m_state.load(std::memory_order_relaxed);

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            LOGI_NET("Gemini Live Engine: Handshake established.");
            self->m_state.store(ConnectionState::CONNECTED, std::memory_order_relaxed);
            self->transmitSetupHandshake();
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::WS_CONNECTED, 0);
            break;
            
        case WEBSOCKET_EVENT_DATA:
            // Opcode 1 = Text Frame, or standard fragmented frame carrying data
            if (data->op_code == 1 || data->data_len > 0) {
                // If this is the start of a completely fresh text payload sequence, clear the old arena
                if (data->payload_offset == 0) {
                    self->m_incoming_assembly_buffer.clear();
                }

                // Append the incoming slice chunk directly into our PSRAM-backed vector array
                size_t current_size = self->m_incoming_assembly_buffer.size();
                self->m_incoming_assembly_buffer.resize(current_size + data->data_len);
                std::memcpy(self->m_incoming_assembly_buffer.data() + current_size, data->data_ptr, data->data_len);

                // Verify if this block completes the full text transmission frame sequence
                if (data->payload_offset + data->data_len >= data->payload_len) {
                    // Inject a clean null terminator safely without resizing errors
                    self->m_incoming_assembly_buffer.push_back('\0');
                    
                    // Route the unified, completed string array straight to our optimization parser
                    self->processIncomingFrame(self->m_incoming_assembly_buffer.data(), self->m_incoming_assembly_buffer.size() - 1);
                }
            }
            break;
            
        case WEBSOCKET_EVENT_DISCONNECTED:
            LOGW_NET("Gemini Live Engine: Connection lost.");
            self->m_state.store(ConnectionState::DISCONNECTED, std::memory_order_relaxed);
            if (previous_state == ConnectionState::CONNECTING) {
                LOGW_NET("Gemini Live Engine: WebSocket disconnected before handshake finished.");
                EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::WS_CONNECT_FAILED, 0);
            } else {
                EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::WS_CLOSED, 0);
            }
            break;
            
        case WEBSOCKET_EVENT_ERROR:
            LOGE_NET("Gemini Live Engine: Hardware Socket Error encountered.");
            self->m_state.store(ConnectionState::ERROR_STATE, std::memory_order_relaxed);
            if (previous_state == ConnectionState::CONNECTING) {
                EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::WS_CONNECT_FAILED, 0);
            } else {
                EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::TRANSPORT_ERROR, 0);
            }
            break;
    }
}

void GeminiProtocolTask::run() {
    LOGI_NET("GeminiProtocolTask active on Core %d", xPortGetCoreID());
    time_t now;
    time(&now);
    LOGI_NET("Current epoch=%lld", (long long)now);
    m_state.store(ConnectionState::DISCONNECTED, std::memory_order_relaxed);


    while (m_running) {
        if (m_connect_requested.load(std::memory_order_relaxed) &&
            m_state.load(std::memory_order_relaxed) != ConnectionState::CONNECTED &&
            m_state.load(std::memory_order_relaxed) != ConnectionState::CONNECTING) {
            startClientConnection();
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Relinquish CPU context slice
    }

    if (m_client) {
        esp_websocket_client_stop(m_client);
        esp_websocket_client_destroy(m_client);
        m_client = nullptr;
    }
}

void GeminiProtocolTask::transmitSetupHandshake() {
    if (!m_client || !esp_websocket_client_is_connected(m_client)) return;
    
    LOGI_NET("Uplinking auto-generated JSON schema compilation payload...");
    esp_websocket_client_send_text(m_client, 
                                   GeminiSkills::SETUP_HANDSHAKE_JSON, 
                                   strlen(GeminiSkills::SETUP_HANDSHAKE_JSON), 
                                   pdMS_TO_TICKS(1000));
}

void GeminiProtocolTask::transmitToolResponse(const char* call_id, const char* json_result) {
    if (!m_client || !esp_websocket_client_is_connected(m_client) || !call_id) return;
    
    cJSON* root = cJSON_CreateObject();
    cJSON* toolResponse = cJSON_CreateObject();
    cJSON* functionResponses = cJSON_CreateArray();
    cJSON* funcResp = cJSON_CreateObject();
    
    cJSON_AddStringToObject(funcResp, "id", call_id);
    
    cJSON* resultObj = json_result ? cJSON_Parse(json_result) : nullptr;
    if (!resultObj) {
        resultObj = cJSON_CreateObject();
    }
    
    cJSON* responseObj = cJSON_CreateObject();
    cJSON_AddItemToObject(responseObj, "output", resultObj);
    cJSON_AddItemToObject(funcResp, "response", responseObj);
    cJSON_AddItemToArray(functionResponses, funcResp);
    cJSON_AddItemToObject(toolResponse, "functionResponses", functionResponses);
    cJSON_AddItemToObject(root, "toolResponse", toolResponse);
    
    char* payload = cJSON_PrintUnformatted(root);
    if (payload) {
        esp_websocket_client_send_text(m_client, payload, strlen(payload), pdMS_TO_TICKS(1000));
        cJSON_free(payload);
    }
    cJSON_Delete(root);
}

void GeminiProtocolTask::transmitAudioUplink(const char* base64_pcm) {
    if (!m_client) {
        static int client_not_ready_count = 0;
        if (++client_not_ready_count % 25 == 1) {
            LOGW_NET("transmitAudioUplink: websocket client handle not ready yet (count=%d)", client_not_ready_count);
        }
        return;
    }
    if (!esp_websocket_client_is_connected(m_client)) {
        static int ws_not_connected_count = 0;
        if (++ws_not_connected_count % 100 == 1) {
            LOGW_NET("transmitAudioUplink: WebSocket NOT connected (count=%d)", ws_not_connected_count);
        }
        return;
    }
    if (!base64_pcm) return;
    
    // Suppress transmitting any microphone audio chunks while Gemini is actively speaking.
    // This entirely eliminates lock contentions and wipes out console "Could not lock ws-client" errors!
    if (WakeWordDetector::getInstance().isAssistantActive()) {
        return;
    }
    
    // Zero-Allocation Direct Formatting inside external PSRAM arena
    int payload_len = snprintf(m_static_payload_arena, 4096, 
                               "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}", 
                               base64_pcm);
                               
    if (payload_len > 0 && payload_len < 4096) {
        // Increased write timeout to 2000ms to allow LwIP TCP buffers to flush over transient Wi-Fi jitter safely
        int ret = esp_websocket_client_send_text(m_client, m_static_payload_arena, payload_len, pdMS_TO_TICKS(2000));
        static int uplink_count = 0;
        if (++uplink_count % 50 == 1) {
            LOGI_NET("Audio uplink #%d (b64_len=%d, send_ret=%d)", uplink_count, (int)strlen(base64_pcm), ret);
        }
    }
}

void GeminiProtocolTask::sendTextDirect(const char* text) {
    if (isConnected() && text) {
        esp_websocket_client_send_text(m_client, text, strlen(text), pdMS_TO_TICKS(1000));
    }
}

void GeminiProtocolTask::processIncomingFrame(char* payload, size_t length) {
    // IN-PLACE OPTIMIZATION: Temporarily terminate the payload string container 
    // to avoid an internal duplicate string allocation loop.
    char old_char = payload[length];
    payload[length] = '\0';

    if (length < 500) {
        LOGI_NET("processIncomingFrame: received %d bytes: %s", (int)length, payload);
    } else {
        LOGI_NET("processIncomingFrame: received %d bytes", (int)length);
    }

    cJSON* root = cJSON_Parse(payload);
    if (!root) {
        LOGW_NET("processIncomingFrame: cJSON_Parse FAILED (first 100 chars: %.100s)", payload);
        payload[length] = old_char; // Restore structure safely
        return;
    }

    cJSON* errorObj = cJSON_GetObjectItem(root, "error");
    if (errorObj) {
        cJSON* codeObj = cJSON_GetObjectItem(errorObj, "code");
        cJSON* messageObj = cJSON_GetObjectItem(errorObj, "message");
        cJSON* statusObj = cJSON_GetObjectItem(errorObj, "status");

        int code = codeObj ? codeObj->valueint : 0;
        const char* message = messageObj ? messageObj->valuestring : "Unknown error";
        const char* status = statusObj ? statusObj->valuestring : "UNKNOWN";

        ESP_LOGE("GeminiError", "==================================================");
        ESP_LOGE("GeminiError", "!!! GEMINI API SERVER ERROR !!!");
        ESP_LOGE("GeminiError", "Status:  %s", status);
        ESP_LOGE("GeminiError", "Code:    %d", code);
        ESP_LOGE("GeminiError", "Message: %s", message);
        if (code == 429) {
            ESP_LOGE("GeminiError", "--------------------------------------------------");
            ESP_LOGE("GeminiError", "--> WARNING: You are being rate-limited by Google AI Studio!");
            ESP_LOGE("GeminiError", "--> Please wait a moment before resuming conversation.");
        }
        ESP_LOGE("GeminiError", "==================================================");

        if (code == 429) {
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::QUOTA_EXCEEDED, code);
        } else {
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::SERVER_ERROR, code);
        }
    }

    cJSON* goAwayObj = cJSON_GetObjectItem(root, "goAway");
    if (goAwayObj) {
        cJSON* timeLeftObj = cJSON_GetObjectItem(goAwayObj, "timeLeft");
        const char* time_left = timeLeftObj ? timeLeftObj->valuestring : "unknown";

        LOGW_NET("Gemini Live Engine: Received 'goAway' signal from server (timeLeft: %s).", time_left);
        LOGW_NET("Gemini Live Engine: Initiating graceful socket closure from our end...");

        m_state.store(ConnectionState::GOING_AWAY, std::memory_order_relaxed);

        EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::GEMINI_GO_AWAY, 0);

        // 2. Send standard WebSocket Close Frame (opcode 0x08) to the server
        if (m_client) {
            esp_websocket_client_close(m_client, pdMS_TO_TICKS(1500));
        }
    }

    cJSON* serverContent = cJSON_GetObjectItem(root, "serverContent");
    if (serverContent) {
        
        // --- 1. Audio Streaming Pipeline Processing ---
        cJSON* modelTurn = cJSON_GetObjectItem(serverContent, "modelTurn");
        if (modelTurn) {
            
            // Process all parts in the model's turn (handles mixed text transcripts and audio data robustly)
            cJSON* parts = cJSON_GetObjectItem(modelTurn, "parts");
            if (parts && cJSON_IsArray(parts)) {
                int parts_count = cJSON_GetArraySize(parts);
                for (int p = 0; p < parts_count; ++p) {
                    cJSON* part = cJSON_GetArrayItem(parts, p);
                    if (!part) continue;

                    // A. Extract and print text responses (what the assistant is saying)
                    cJSON* textObj = cJSON_GetObjectItem(part, "text");
                    if (textObj && cJSON_IsString(textObj)) {
                        ESP_LOGW("GeminiText", ">>> ASSISTANT: %s", textObj->valuestring);
                    }

                    // B. Extract and process audio streaming payload
                    cJSON* inlineData = cJSON_GetObjectItem(part, "inlineData");
                    if (inlineData) {
                        // Fire ASSISTANT_TALKING event on the absolute first audio packet
                        if (!g_assistant_currently_talking.load(std::memory_order_relaxed)) {
                            AudioService::getInstance().enterAssistantPlaybackModeNow();
                            g_assistant_currently_talking.store(true, std::memory_order_relaxed);
                            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::ASSISTANT_AUDIO_STARTED, 0);
                        }

                        cJSON* data = cJSON_GetObjectItem(inlineData, "data");
                        if (data && cJSON_IsString(data)) {
                            const char* base64_pcm = data->valuestring;
                            size_t b64_len = strlen(base64_pcm);
                            size_t pcm_out_len = 0;
                            
                            // Dry-run sizing extraction
                            mbedtls_base64_decode(nullptr, 0, &pcm_out_len, 
                                                 reinterpret_cast<const unsigned char*>(base64_pcm), b64_len);
                                                 
                            if (pcm_out_len > 0 && pcm_out_len <= STATIC_PCM_ARENA_MAX_SIZE) {
                                size_t written = 0;
                                if (mbedtls_base64_decode(m_static_pcm_scratch_arena, pcm_out_len, &written, 
                                                         reinterpret_cast<const unsigned char*>(base64_pcm), b64_len) == 0) {
                                    // Native 24kHz Playback (Zero Downsampling Overhead!):
                                    // We push the decoded 24kHz PCM directly to the speaker queue.
                                    if (written > 0) {
                                        BufferManager::getInstance().send(Buffers::SPK_RX_BUF, m_static_pcm_scratch_arena, written, pdMS_TO_TICKS(20));
                                        
                                        // Update activity timeout supervisor
                                        AudioService::getInstance().updateActivity();
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // B. Parse Turn Conclusion (End Speaking) — at serverContent level per API docs
        cJSON* turnComplete = cJSON_GetObjectItem(serverContent, "turnComplete");
        if (turnComplete && cJSON_IsTrue(turnComplete)) {
            g_assistant_currently_talking.store(false, std::memory_order_relaxed);
            LOGI_NET("Assistant turn complete");
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::ASSISTANT_TURN_COMPLETE, 0);
        }

        // C. Parse Interruption Signal (Barge-In detected by server-side VAD)
        cJSON* interruptedObj = cJSON_GetObjectItem(serverContent, "interrupted");
        if (interruptedObj && cJSON_IsTrue(interruptedObj)) {
            g_assistant_currently_talking.store(false, std::memory_order_relaxed);
            LOGI_NET("Server detected barge-in — flushing speaker buffer");
            BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);
            EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::ASSISTANT_TURN_COMPLETE, 0);
        }
        
        // --- 2. Asynchronous Hardware Tool Call Processing ---
        cJSON* toolCall = cJSON_GetObjectItem(serverContent, "toolCall");
        if (toolCall) {
            cJSON* functionCalls = cJSON_GetObjectItem(toolCall, "functionCalls");
            if (functionCalls && cJSON_IsArray(functionCalls)) {
                cJSON* funcCall = cJSON_GetArrayItem(functionCalls, 0);
                if (funcCall) {
                    cJSON* nameObj = cJSON_GetObjectItem(funcCall, "name");
                    cJSON* idObj = cJSON_GetObjectItem(funcCall, "id");
                    cJSON* argsObj = cJSON_GetObjectItem(funcCall, "args");
                    
                    if (nameObj && idObj && argsObj) {
                        // Wipe our persistent internal member variable cleanly
                        std::memset(&m_static_skill_event_slot, 0, sizeof(m_static_skill_event_slot));
                        std::strncpy(m_static_skill_event_slot.call_id, idObj->valuestring, sizeof(m_static_skill_event_slot.call_id) - 1);
                        
                        if (GeminiSkills::decode_incoming_arguments(nameObj->valuestring, argsObj, m_static_skill_event_slot)) {
                            LOGI_NET("Tool request successfully isolated: %s", nameObj->valuestring);
                            EventBus::getInstance().publish(APP_EVENTS, AppEvent::GEMINI_TOOL_CALL, &m_static_skill_event_slot);
                        }
                    }
                }
            }
        }
    }
    
    cJSON_Delete(root);
    payload[length] = old_char; // Restore standard string profile state
}

void GeminiProtocolTask::connect() {
    ConnectionState current_state = m_state.load(std::memory_order_relaxed);

    if (current_state == ConnectionState::CONNECTED ||
        current_state == ConnectionState::CONNECTING) {
        LOGI_NET("Gemini Live Engine: Bypassing connect, already state %d", (int)current_state);
        return;
    }

    m_connect_requested.store(true, std::memory_order_relaxed);
    if (!ensureClientInitialized()) {
        LOGE_NET("Gemini Live Engine: WebSocket client initialization failed. Connect request cannot proceed.");
        EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::WS_CONNECT_FAILED, 0);
        return;
    }

    if (!m_client) {
        LOGW_NET("Gemini Live Engine: Connect requested before websocket client initialization completed. Queuing request...");
        return;
    }

    if (!startClientConnection()) {
        EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::WS_CONNECT_FAILED, 0);
    }
}

void GeminiProtocolTask::closeConnection() {
    ConnectionState current_state = m_state.load(std::memory_order_relaxed);
    m_connect_requested.store(false, std::memory_order_relaxed);
    g_assistant_currently_talking.store(false, std::memory_order_relaxed);

    if (current_state != ConnectionState::DISCONNECTED) {
        LOGI_NET("Gemini Live Engine: Closing connection...");

        if (m_client) {
            m_state.store(ConnectionState::DISCONNECTED, std::memory_order_relaxed);
            esp_websocket_client_close(m_client, pdMS_TO_TICKS(1000));
            esp_websocket_client_stop(m_client);
        }
    }
}

bool GeminiProtocolTask::startClientConnection() {
    if (!m_client) {
        LOGE_NET("Cannot start Gemini websocket connection because the client handle is null.");
        return false;
    }

    LOGI_NET("Gemini Live Engine: Connecting to WebSocket...");
    m_state.store(ConnectionState::CONNECTING, std::memory_order_relaxed);
    g_assistant_currently_talking.store(false, std::memory_order_relaxed);
    esp_websocket_client_stop(m_client);
    esp_err_t start_err = esp_websocket_client_start(m_client);
    if (start_err != ESP_OK) {
        LOGE_NET("esp_websocket_client_start failed with err=0x%x", (unsigned)start_err);
        m_state.store(ConnectionState::ERROR_STATE, std::memory_order_relaxed);
        return false;
    }

    m_connect_requested.store(false, std::memory_order_relaxed);
    return true;
}

bool GeminiProtocolTask::ensureClientInitialized() {
    std::lock_guard<std::mutex> lock(m_client_mutex);
    if (m_client) {
        return true;
    }

    m_ws_uri = GEMINI_LIVE_BASE_URL;
#ifdef CONFIG_GEMINI_API_KEY
    if (std::strlen(CONFIG_GEMINI_API_KEY) == 0) {
        LOGE_NET("CONFIG_GEMINI_API_KEY is defined but empty. Gemini websocket cannot be started.");
        return false;
    }
    m_ws_uri += CONFIG_GEMINI_API_KEY;
#else
    LOGE_NET("CONFIG_GEMINI_API_KEY is missing. Define it in menuconfig/Kconfig before using Gemini Live.");
    return false;
#endif

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = m_ws_uri.c_str();
    ws_cfg.buffer_size = 4096;
    ws_cfg.reconnect_timeout_ms = 10000;
    ws_cfg.network_timeout_ms = 10000;
    ws_cfg.task_stack = 10240;

#ifdef USE_PRODUCTION_SECURE_TLS
    ws_cfg.cert_pem = GLOBAL_SIGN_R3_CA_PEM;
    ws_cfg.crt_bundle_attach = nullptr;
#else
    ws_cfg.cert_pem = nullptr;
    ws_cfg.crt_bundle_attach = nullptr;
#endif

    m_client = esp_websocket_client_init(&ws_cfg);
    if (!m_client) {
        LOGE_NET("Failed to initialize Gemini websocket client handle.");
        return false;
    }

    esp_websocket_register_events(m_client, WEBSOCKET_EVENT_ANY, websocketEventHandler, this);
    LOGI_NET("Gemini websocket client initialized successfully.");
    return true;
}
