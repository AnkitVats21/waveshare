#pragma once

#include "common/TaskBase.h"
#include "esp_websocket_client.h"
#include "gemini_skills_generated.h"
#include "esp_heap_caps.h"
#include <vector>
#include <new>

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

/**
 * @brief Pinned to Core 0, handles WebSocket connections, cJSON parsing, 
 * and schema orchestration for Gemini API tool calls.
 */
class GeminiProtocolTask : public TaskBase {
public:
    static GeminiProtocolTask& getInstance();

    void transmitToolResponse(const char* call_id, const char* json_result);
    void transmitAudioUplink(const char* base64_pcm);
    
    bool isConnected() { return m_client != nullptr && esp_websocket_client_is_connected(m_client); }
    void sendTextDirect(const char* text);

protected:
    void run() override;

private:
    GeminiProtocolTask(const Config& cfg);
    ~GeminiProtocolTask() = default;

    void transmitSetupHandshake();
    void processIncomingFrame(char* payload, size_t length);

    static void websocketEventHandler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

    esp_websocket_client_handle_t m_client = nullptr;

    // Persistent Zero-Allocation Arenas for Audio & Skill Tool execution
    uint8_t* m_static_pcm_scratch_arena = nullptr;
    uint8_t* m_static_pcm_downsampled_arena = nullptr;
    char* m_static_payload_arena = nullptr;
    static constexpr size_t STATIC_PCM_ARENA_MAX_SIZE = 24576; // 24KB max decoded output ceiling
    GeminiSkills::DecodedSkillCall m_static_skill_event_slot;

    // Fragment assembly buffer explicitly stored in PSRAM
    std::vector<char, PsramAllocator<char>> m_incoming_assembly_buffer;
};
