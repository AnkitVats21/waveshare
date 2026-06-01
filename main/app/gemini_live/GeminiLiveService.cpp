#include "GeminiLiveService.h"
#include "GeminiAudioPumpTask.h"
#include "GeminiProtocolTask.h"
#include "common/AppLogger.h"

GeminiLiveService::GeminiLiveService() : IService("GeminiLiveService") {}

GeminiLiveService& GeminiLiveService::getInstance() {
    static GeminiLiveService instance;
    return instance;
}

bool GeminiLiveService::onStart() {
    LOGI_SYSTEM("Starting Gemini Live Transports...");
    
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

void GeminiLiveService::sendToolExecutionReceipt(const char* call_id, const char* json_result) {
    // This will be forwarded to the GeminiProtocolTask to encapsulate and transmit.
    // We implement it here to maintain GeminiLiveService as the public API facade.
    // For now, we'll directly call into GeminiProtocolTask.
    GeminiProtocolTask::getInstance().transmitToolResponse(call_id, json_result);
}

void GeminiLiveService::sendInterruptionSignal() {
    // No-op: Gemini Live API handles barge-in automatically from the raw microphone stream.
    // Sending an explicit "interrupted" field violates the client schema and causes server disconnect.
    ESP_LOGI("GeminiLive", "Barge-In: Local playback stopped (Gemini will auto-interrupt upon raw audio reception).");
}
