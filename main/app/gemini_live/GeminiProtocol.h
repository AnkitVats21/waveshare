#pragma once

#include "common/ReactorTask.h"
#include "WssClient.h"
#include "gemini_skills_generated.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include <new>
#include <mutex>
#include <string>

class GeminiProtocol : public ReactorTask {
public:
    static GeminiProtocol& getInstance();

    typedef void (*ToolCallHandlerFn)(const GeminiSkills::DecodedSkillCall& skill_call, void* ctx);

    void setToolCallHandler(ToolCallHandlerFn handler, void* ctx) {
        m_tool_handler = handler;
        m_tool_ctx = ctx;
    }

    void transmitToolResponse(const char* call_id, const char* json_result);
    void transmitAudioUplink(const char* base64_pcm);
    
    bool isConnected() { return m_client.isConnected(); }
    void connect();
    void closeConnection();
    void forceReconnect() { connect(); }
    void sendTextDirect(const char* text);

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    GeminiProtocol();
    ~GeminiProtocol() override;

    bool ensureClientInitialized();
    void transmitSetupHandshake();
    void processIncomingFrame(char* payload, size_t length);
    void handleToolCall(JsonObjectConst toolCall);

    static void websocketEventHandler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
    bool startClientConnection();

    WssClient m_client;
    std::mutex m_client_mutex;
    std::string m_ws_uri;

    ToolCallHandlerFn m_tool_handler = nullptr;
    void* m_tool_ctx = nullptr;

    // Persistent Zero-Allocation Arenas for Audio & Skill Tool execution
    uint8_t* m_static_pcm_scratch_arena = nullptr;
    char* m_static_payload_arena = nullptr;
    // static constexpr size_t STATIC_PCM_ARENA_MAX_SIZE = 65536; // 64KB max decoded output ceiling
    GeminiSkills::DecodedSkillCall m_static_skill_event_slot;

    // Fixed-size memory management variables in PSRAM
    static constexpr size_t PSRAM_RB_SIZE = 512 * 1024;      // 512KB static ring buffer pool
    static constexpr size_t MAX_INCOMING_FRAME_SIZE = 98304; // 96KB max single frame staging space

    RingbufHandle_t m_incoming_psram_rb = nullptr;
    uint8_t* m_assembly_scratch = nullptr;
    size_t m_assembly_idx = 0;
    bool m_frame_overflowed = false;

    // Session-specific diagnostics statistics
    uint32_t m_rx_frames = 0;
    uint32_t m_rx_dropped_frames = 0;
    uint32_t m_rx_audio_bytes = 0;

    static constexpr const char* TAG = "GeminiProto";
};


