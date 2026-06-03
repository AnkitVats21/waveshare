#include "GeminiAudioPumpTask.h"
#include "GeminiProtocolTask.h"
#include "GeminiLiveService.h"
#include "common/AppLogger.h"
#include "services/BufferManager.h"
#include "app/audio/MicCapture.h"
#include "app/audio/SpeakerPlayback.h"
#include "app/audio/AudioService.h"
#include "mbedtls/base64.h"
#include "app/wake_word/WakeWordDetector.h"
#include <cstring>
#include <string>
#include <vector>

GeminiAudioPumpTask::GeminiAudioPumpTask(const Config& cfg) : TaskBase(cfg) {
    // Allocate 2KB persistent external PSRAM arena for base64 transcoding
    m_static_b64_arena = static_cast<char*>(
        heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    assert(m_static_b64_arena != nullptr);
}

GeminiAudioPumpTask& GeminiAudioPumpTask::getInstance() {
    static Config default_config = {
        .name = "GeminiAudioPumpTask",
        .stack_size = 12288,
        .priority = 5,
        .core_id = 1 // Bound to Core 1 for DMA locality
    };
    static GeminiAudioPumpTask instance(default_config);
    return instance;
}

void GeminiAudioPumpTask::run() {
    LOGI_AUDIO("GeminiAudioPumpTask running on Core %d", xPortGetCoreID());

    while (m_running) {
        bool processed = processUplink();
        vTaskDelay(pdMS_TO_TICKS(processed ? 30 : 10));
    }
}

bool GeminiAudioPumpTask::processUplink() {
    auto& bm = BufferManager::getInstance();
    size_t chunk_size = 0;

    // Non-blocking read from Mic capture buffer (returns 1024 bytes usually)
    uint8_t* pcm_data = static_cast<uint8_t*>(
        bm.receive(Buffers::MIC_TX_BUF, &chunk_size, 0, 1024));

    if (pcm_data != nullptr) {
        static int pump_read_count = 0;
        bool streaming    = WakeWordDetector::getInstance().isStreamingActive();
        // ── KEY GATE ──────────────────────────────────────────────────────────
        // Only transmit when the WebSocket is fully connected.
        // While the socket is still handshaking (Connecting state) we drain the
        // ring buffer silently so it never overflows, but we do NOT attempt to
        // send anything — this eliminates the "m_client is null" warning storm.
        bool ws_connected = GeminiProtocolTask::getInstance().isConnected();

        if (++pump_read_count % 200 == 1) {
            LOGI_AUDIO("PumpTask: read #%d, chunk=%d bytes, streaming=%d, ws=%d",
                       pump_read_count, (int)chunk_size,
                       streaming ? 1 : 0, ws_connected ? 1 : 0);
        }

        // Transmit only when wake-word session is active AND WebSocket is live
        if (streaming && ws_connected) {
            size_t written = 0;
            // Transcode directly into persistent external PSRAM arena (zero heap allocation!)
            if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(m_static_b64_arena),
                                      2048, &written, pcm_data, chunk_size) == 0) {
                m_static_b64_arena[written] = '\0';
                GeminiProtocolTask::getInstance().transmitAudioUplink(m_static_b64_arena);
            }
        }

        // Always return the item — prevents ring buffer leaks regardless of WS state
        bm.returnItem(Buffers::MIC_TX_BUF, pcm_data);
        return true;
    }
    return false;
}
