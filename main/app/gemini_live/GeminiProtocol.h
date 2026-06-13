#pragma once

#include "common/ReactorTask.h"
#include "WssClient.h"
#include "gemini_skills_generated.h"
#include "esp_heap_caps.h"
#include <vector>
#include <new>
#include <mutex>
#include <string>

// Custom Allocator to force std::vector dynamic reallocations directly into external 8MB PSRAM
template <typename T>
struct PsramAllocator {
    using value_type = T;
    PsramAllocator() = default;
    template <class U> constexpr PsramAllocator(const PsramAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        if (n > std::size_t(-1) / sizeof(T)) throw std::bad_alloc();
        T* p = static_cast<T*>(heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!p) throw std::bad_alloc();
        return p;
    }
    void deallocate(T* p, std::size_t) noexcept {
        heap_caps_free(p);
    }
};

template <typename T, typename U>
bool operator==(const PsramAllocator<T>&, const PsramAllocator<U>&) { return true; }
template <typename T, typename U>
bool operator!=(const PsramAllocator<T>&, const PsramAllocator<U>&) { return false; }

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
    ~GeminiProtocol() override = default;

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
    static constexpr size_t STATIC_PCM_ARENA_MAX_SIZE = 24576; // 24KB max decoded output ceiling
    GeminiSkills::DecodedSkillCall m_static_skill_event_slot;

    // Fragment assembly buffer explicitly stored in PSRAM
    std::vector<char, PsramAllocator<char>> m_incoming_assembly_buffer;

    static constexpr const char* TAG = "GeminiProto";
};


