#include "GeminiProtocolTask.h"
#include "GeminiAudioPumpTask.h"
#include "gemini_skills_generated.h"
#include "common/AppLogger.h"
#include "app/event/EventBus.h"
#include "common/events/app_events.h"
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

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            LOGI_NET("Gemini Live Engine: Handshake established.");
            self->transmitSetupHandshake();
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
            break;
            
        case WEBSOCKET_EVENT_ERROR:
            LOGE_NET("Gemini Live Engine: Hardware Socket Error encountered.");
            break;
    }
}

void GeminiProtocolTask::run() {
    LOGI_NET("GeminiProtocolTask active on Core %d", xPortGetCoreID());

    std::string ws_uri = GEMINI_LIVE_BASE_URL;
#ifdef CONFIG_GEMINI_API_KEY
    ws_uri += CONFIG_GEMINI_API_KEY;
#else
    LOGE_NET("API Key missing! Define CONFIG_GEMINI_API_KEY in Kconfig.");
    vTaskDelete(nullptr);
    return;
#endif

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = ws_uri.c_str();
    ws_cfg.buffer_size = 4096; // Transient chunk fragmentation framing buffer size to prevent DRAM exhaustion
    ws_cfg.reconnect_timeout_ms = 10000; // Silence reconnection timeout warnings
    ws_cfg.network_timeout_ms = 10000;   // Silence network timeout warnings
    ws_cfg.task_stack = 10240; // Allocate 10KB stack for websocket_task to prevent stack overflow from callbacks
    
    // Toggleable Secure TLS Verification (Production) vs Direct-Trust Testing (No Verification)
#ifdef USE_PRODUCTION_SECURE_TLS
    ws_cfg.cert_pem = GLOBAL_SIGN_R3_CA_PEM;
    ws_cfg.crt_bundle_attach = nullptr;
#else
    // Skip server certificate verification (requires CONFIG_ESP_TLS_INSECURE=y 
    // and CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y in sdkconfig)
    ws_cfg.cert_pem = nullptr;
    ws_cfg.crt_bundle_attach = nullptr;
#endif

    m_client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(m_client, WEBSOCKET_EVENT_ANY, websocketEventHandler, this);
    time_t now;
    time(&now);
    LOGI_NET("Current epoch=%lld", (long long)now);
    esp_websocket_client_start(m_client);

    while (m_running) {
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
        LOGW_NET("transmitAudioUplink: m_client is null");
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
    LOGI_NET("processIncomingFrame: received %d bytes", (int)length);
    
    // IN-PLACE OPTIMIZATION: Temporarily terminate the payload string container 
    // to avoid an internal duplicate string allocation loop.
    char old_char = payload[length];
    payload[length] = '\0';

    cJSON* root = cJSON_Parse(payload);
    if (!root) {
        LOGW_NET("processIncomingFrame: cJSON_Parse FAILED (first 100 chars: %.100s)", payload);
        payload[length] = old_char; // Restore structure safely
        return;
    }

    cJSON* serverContent = cJSON_GetObjectItem(root, "serverContent");
    if (serverContent) {
        
        // Track whether assistant is actively streaming audio
        static bool assistant_currently_talking = false;

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
                        if (!assistant_currently_talking) {
                            assistant_currently_talking = true;
                            // Defer VAD silence counters locally; turn on Speaking LEDs
                            EventBus::getInstance().publish(APP_EVENTS, AppEvent::ASSISTANT_TALKING, 0);
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
                                    // Gemini outputs 24kHz mono PCM. Resample to 16kHz before pushing to SPK_RX_BUF.
                                    int src_samples = written / sizeof(int16_t);
                                    const int16_t* src_buf = reinterpret_cast<const int16_t*>(m_static_pcm_scratch_arena);
                                    
                                    int max_dest_samples = STATIC_PCM_ARENA_MAX_SIZE / sizeof(int16_t);
                                    int16_t* dest_buf = reinterpret_cast<int16_t*>(m_static_pcm_downsampled_arena);
                                    int out_idx = 0;
                                    
                                    for (int i = 0; i < src_samples - 2; i += 3) {
                                        if (out_idx + 1 < max_dest_samples) {
                                            dest_buf[out_idx++] = src_buf[i];
                                            dest_buf[out_idx++] = (int16_t)(((int32_t)src_buf[i+1] + (int32_t)src_buf[i+2]) >> 1);
                                        }
                                    }
                                    
                                    size_t dest_bytes = out_idx * sizeof(int16_t);
                                    if (dest_bytes > 0) {
                                        BufferManager::getInstance().send(Buffers::SPK_RX_BUF, dest_buf, dest_bytes, pdMS_TO_TICKS(20));
                                        
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
            assistant_currently_talking = false;
            LOGI_NET("Assistant turn complete");
            EventBus::getInstance().publish(APP_EVENTS, AppEvent::ASSISTANT_TURN_COMPLETE, 0);
        }

        // C. Parse Interruption Signal (Barge-In detected by server-side VAD)
        cJSON* interruptedObj = cJSON_GetObjectItem(serverContent, "interrupted");
        if (interruptedObj && cJSON_IsTrue(interruptedObj)) {
            assistant_currently_talking = false;
            LOGI_NET("Server detected barge-in — flushing speaker buffer");
            BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);
            EventBus::getInstance().publish(APP_EVENTS, AppEvent::ASSISTANT_TURN_COMPLETE, 0);
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
