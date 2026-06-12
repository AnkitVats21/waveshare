#include "GeminiLiveService.h"
#include "GeminiAudioPumpTask.h"
#include "GeminiProtocolTask.h"
#include "common/AppLogger.h"
#include "app/assistant/AssistantEvents.h"
#include "esp_heap_caps.h"
#include <cstring>

GeminiLiveService::GeminiLiveService() : IService("GeminiLiveService") {}

GeminiLiveService& GeminiLiveService::getInstance() {
    static GeminiLiveService instance;
    return instance;
}

bool GeminiLiveService::onStart() {
    LOGI_SYSTEM("Starting Gemini Live Transports...");
    
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::TRANSPORT_CONNECT);
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::TRANSPORT_CLOSE);

    // Start Core 0 WebSocket Protocol handler
    GeminiProtocolTask::getInstance().start();
    
    // Start Core 1 High-Frequency Audio Pipeline
    GeminiAudioPumpTask::getInstance().start();
    
    return true;
}

void GeminiLiveService::onStop() {
    LOGI_SYSTEM("Stopping Gemini Live Transports...");
    GeminiProtocolTask::getInstance().stop();
    GeminiAudioPumpTask::getInstance().stop();
}

void GeminiLiveService::onEvent(esp_event_base_t base, int32_t id, void* /*data*/) {
    if (base == ASSISTANT_EVENTS) {
        auto event = static_cast<AssistantEvent>(id);
        if (event == AssistantEvent::TRANSPORT_CONNECT) {
            GeminiProtocolTask::getInstance().connect();
        } else if (event == AssistantEvent::TRANSPORT_CLOSE) {
            GeminiProtocolTask::getInstance().closeConnection();
        }
    }
}

void GeminiLiveService::sendToolExecutionReceipt(const char* call_id, const char* json_result) {
    // This will be forwarded to the GeminiProtocolTask to encapsulate and transmit.
    // We implement it here to maintain GeminiLiveService as the public API facade.
    // For now, we'll directly call into GeminiProtocolTask.
    GeminiProtocolTask::getInstance().transmitToolResponse(call_id, json_result);
}

void GeminiLiveService::sendInterruptionSignal() {
    // Simplified: No interruption support in stable half-duplex mode
}

void GeminiLiveService::forceReconnect() {
    GeminiProtocolTask::getInstance().connect();
}

void GeminiLiveService::submitAudioUplink(const char* base64_pcm) {
    if (!base64_pcm || base64_pcm[0] == '\0') {
        return;
    }

    if (GeminiProtocolTask::getInstance().isConnected()) {
        GeminiProtocolTask::getInstance().transmitAudioUplink(base64_pcm);
    }
}
