#include "GeminiAudioPump.h"
#include "GeminiProtocol.h"
#include "common/AppLogger.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include "services/BufferManager.h"
#include "app/audio/MicCapture.h"
#include "app/audio/SpeakerPlayback.h"
#include "app/wake_word/WakeWordEngine.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

GeminiAudioPump::GeminiAudioPump(const Config& cfg)
    : TaskBase(cfg)
{
    // Allocate 2KB persistent external PSRAM arena for base64 transcoding
    m_static_b64_arena = static_cast<char*>(
        heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    assert(m_static_b64_arena != nullptr);
}

GeminiAudioPump& GeminiAudioPump::getInstance() {
    static Config default_config = {
        .name = "GeminiAudioPump",
        .stack_size = ThreadConfig::StackSize::STACK_NORMAL,
        .priority = ThreadConfig::Priority::AUDIO_PUMP,
        .core_id = ThreadConfig::CORE_AUDIO
    };
    static GeminiAudioPump instance(default_config);
    return instance;
}

void GeminiAudioPump::run() {
    LOGI_AUDIO("GeminiAudioPump running on Core %d", xPortGetCoreID());

    // Register as a reactor for changes to pipeline and assistant states
    EmbeddedSysDb::getInstance().registerReactor(COMP::PIPELINE | COMP::ASSISTANT, m_task_handle);

    // Initialize cache
    m_cached_ws_state = EmbeddedSysDb::getInstance().wsState();
    m_cached_pipeline_mode = EmbeddedSysDb::getInstance().pipelineMode();

    while (m_running) {
        bool processed = processUplink();
        if (!processed) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

bool GeminiAudioPump::processUplink() {
    auto& bm = BufferManager::getInstance();
    size_t chunk_size = 0;

    // Block on receive with a 100ms timeout
    uint8_t* pcm_data = static_cast<uint8_t*>(
        bm.receive(Buffers::MIC_TX_BUF, &chunk_size, pdMS_TO_TICKS(100), 1024));

    if (pcm_data != nullptr) {
        static int pump_read_count = 0;
        bool streaming = WakeWordEngine::getInstance().isStreamingActive();
        
        // Check for state change notifications from EmbeddedSysDb (zero-lock check)
        if (ulTaskNotifyTake(pdTRUE, 0) != 0) {
            m_cached_ws_state = EmbeddedSysDb::getInstance().wsState();
            m_cached_pipeline_mode = EmbeddedSysDb::getInstance().pipelineMode();
        }

        bool ws_connected = (m_cached_ws_state == WsState::CONNECTED);
        bool live_mode    = (m_cached_pipeline_mode == PipelineMode::GEMINI_LIVE);

        if (++pump_read_count % 200 == 1) {
            LOGI_AUDIO("AudioPump: chunk=%d bytes, streaming=%d, ws=%d, live_mode=%d",
                       (int)chunk_size, streaming ? 1 : 0, ws_connected ? 1 : 0, live_mode ? 1 : 0);
        }

        // Transmit only in GEMINI_LIVE mode, when streaming is active, and WebSocket is connected
        if (live_mode && streaming && ws_connected) {
            size_t written = 0;
            if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(m_static_b64_arena),
                                      2048, &written, pcm_data, chunk_size) == 0) {
                m_static_b64_arena[written] = '\0';
                GeminiProtocol::getInstance().transmitAudioUplink(m_static_b64_arena);
            } else {
                LOGE_AUDIO("Base64 encoding failed.");
            }
        }

        // Always return the item
        bm.returnItem(Buffers::MIC_TX_BUF, pcm_data);
        return true;
    }
    return false;
}

