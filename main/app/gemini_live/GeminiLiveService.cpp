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
    subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::TRANSPORT_SEND_BUFFERED_AUDIO);

    if (!m_buffered_audio_arena) {
        m_buffered_audio_arena = static_cast<char*>(
            heap_caps_malloc(MAX_BUFFERED_AUDIO_CHUNKS * MAX_BASE64_AUDIO_CHUNK,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        );
        if (!m_buffered_audio_arena) {
            LOGE_SYSTEM("Failed to allocate Gemini pre-connect audio buffer.");
            return false;
        }
        std::memset(m_buffered_audio_arena, 0,
                    MAX_BUFFERED_AUDIO_CHUNKS * MAX_BASE64_AUDIO_CHUNK);
    }

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
            clearBufferedAudio();
            GeminiProtocolTask::getInstance().closeConnection();
        } else if (event == AssistantEvent::TRANSPORT_SEND_BUFFERED_AUDIO) {
            flushBufferedAudio();
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
    // No-op: Gemini Live API handles barge-in automatically from the raw microphone stream.
    // Sending an explicit "interrupted" field violates the client schema and causes server disconnect.
    ESP_LOGI("GeminiLive", "Barge-In: Local playback stopped (Gemini will auto-interrupt upon raw audio reception).");
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
        return;
    }

    bufferAudioChunk(base64_pcm);
}

char* GeminiLiveService::getBufferedChunkSlot(size_t index) {
    return m_buffered_audio_arena + (index * MAX_BASE64_AUDIO_CHUNK);
}

void GeminiLiveService::bufferAudioChunk(const char* base64_pcm) {
    std::lock_guard<std::mutex> lock(m_audio_buffer_mutex);
    static uint32_t overflow_warn_count = 0;
    if (!m_buffered_audio_arena) {
        LOGW_AUDIO("Gemini pre-connect audio buffer is not initialized yet; dropping chunk.");
        return;
    }

    size_t slot_index = (m_buffered_head + m_buffered_count) % MAX_BUFFERED_AUDIO_CHUNKS;
    if (m_buffered_count == MAX_BUFFERED_AUDIO_CHUNKS) {
        slot_index = m_buffered_head;
        m_buffered_head = (m_buffered_head + 1) % MAX_BUFFERED_AUDIO_CHUNKS;
        if (++overflow_warn_count % 25 == 1) {
            LOGW_AUDIO("Gemini pre-connect audio buffer full; overwriting oldest chunk (count=%u).",
                       static_cast<unsigned>(overflow_warn_count));
        }
    } else {
        ++m_buffered_count;
    }

    char* slot = getBufferedChunkSlot(slot_index);
    std::strncpy(slot, base64_pcm, MAX_BASE64_AUDIO_CHUNK - 1);
    slot[MAX_BASE64_AUDIO_CHUNK - 1] = '\0';
}

void GeminiLiveService::flushBufferedAudio() {
    std::lock_guard<std::mutex> lock(m_audio_buffer_mutex);
    if (!GeminiProtocolTask::getInstance().isConnected()) {
        LOGW_NET("Requested buffered audio flush before WebSocket was connected. Holding %u buffered chunks.",
                 static_cast<unsigned>(m_buffered_count));
        return;
    }

    if (m_buffered_count == 0) {
        LOGI_NET("No buffered pre-connect audio to flush.");
        return;
    }

    LOGI_NET("Flushing %u buffered pre-connect audio chunks to Gemini.", static_cast<unsigned>(m_buffered_count));
    for (size_t i = 0; i < m_buffered_count; ++i) {
        size_t slot_index = (m_buffered_head + i) % MAX_BUFFERED_AUDIO_CHUNKS;
        GeminiProtocolTask::getInstance().transmitAudioUplink(getBufferedChunkSlot(slot_index));
    }

    m_buffered_head = 0;
    m_buffered_count = 0;
}

void GeminiLiveService::clearBufferedAudio() {
    std::lock_guard<std::mutex> lock(m_audio_buffer_mutex);
    m_buffered_head = 0;
    m_buffered_count = 0;
}
