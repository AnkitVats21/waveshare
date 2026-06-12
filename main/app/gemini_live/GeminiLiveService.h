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

    // Singleton constraints
    GeminiLiveService(const GeminiLiveService&) = delete;
    GeminiLiveService& operator=(const GeminiLiveService&) = delete;
};
