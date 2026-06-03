#pragma once

#include "common/IService.h"
#include "app/event/EventBus.h"
#include <cstddef>
#include <mutex>

/**
 * @brief Orchestrates the Gemini Live edge-to-cloud bidirectional connection.
 */
class GeminiLiveService : public IService {
public:
    static GeminiLiveService& getInstance();
    
    bool onStart() override;
    void onStop() override;
    void onEvent(esp_event_base_t base, int32_t id, void* data) override;

    void sendToolExecutionReceipt(const char* call_id, const char* json_result);
    void sendInterruptionSignal();
    void forceReconnect();
    void submitAudioUplink(const char* base64_pcm);

private:
    GeminiLiveService();
    ~GeminiLiveService() = default;

    void bufferAudioChunk(const char* base64_pcm);
    void flushBufferedAudio();
    void clearBufferedAudio();
    char* getBufferedChunkSlot(size_t index);

    // Singleton constraints
    GeminiLiveService(const GeminiLiveService&) = delete;
    GeminiLiveService& operator=(const GeminiLiveService&) = delete;

    std::mutex m_audio_buffer_mutex;
    char* m_buffered_audio_arena = nullptr;
    size_t m_buffered_head = 0;
    size_t m_buffered_count = 0;

    static constexpr size_t MAX_BUFFERED_AUDIO_CHUNKS = 128;
    static constexpr size_t MAX_BASE64_AUDIO_CHUNK = 2048;
};
