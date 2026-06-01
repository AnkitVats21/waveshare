#pragma once

#include "common/IService.h"
#include "app/event/EventBus.h"

/**
 * @brief Orchestrates the Gemini Live edge-to-cloud bidirectional connection.
 */
class GeminiLiveService : public IService {
public:
    static GeminiLiveService& getInstance();
    
    bool onStart() override;
    void onStop() override;

    void sendToolExecutionReceipt(const char* call_id, const char* json_result);
    void sendInterruptionSignal();

private:
    GeminiLiveService();
    ~GeminiLiveService() = default;

    // Singleton constraints
    GeminiLiveService(const GeminiLiveService&) = delete;
    GeminiLiveService& operator=(const GeminiLiveService&) = delete;
};
