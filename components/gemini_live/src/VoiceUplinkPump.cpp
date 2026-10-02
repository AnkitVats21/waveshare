#include "VoiceUplinkPump.h"
#include "VoiceAgent.h"
#include "common/AppLogger.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include "services/BufferManager.h"
#include "app/audio/MicCapture.h"
#include "app/audio/SpeakerPlayback.h"
#include "app/wake_word/WakeWordEngine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

static auto &sysdb = EmbeddedSysDb::getInstance();

VoiceUplinkPump::VoiceUplinkPump(const Config& cfg)
    : TaskBase(cfg)
{
}

VoiceUplinkPump& VoiceUplinkPump::getInstance() {
    static Config default_config = {
        .name = "VoiceUplinkPump",
        .stack_size = ThreadConfig::StackSize::STACK_NORMAL,
        .priority = ThreadConfig::Priority::AUDIO_PUMP,
        // Core 0: each chunk is a synchronous send on the backend's
        // connection (for Gemini base64 + JSON + TLS), i.e. network work;
        // keep it off Core 1, which carries the AFE feed (AEC+BSS).
        .core_id = ThreadConfig::CORE_NETWORK
    };
    static VoiceUplinkPump instance(default_config);
    return instance;
}

bool VoiceUplinkPump::start() {
    if (!TaskBase::start() || m_task_handle == nullptr) {
        return false;
    }
    sysdb.registerReactor(COMP::PIPELINE | COMP::ASSISTANT, m_task_handle);
    return true;
}

void VoiceUplinkPump::run() {
    LOGI_AUDIO("VoiceUplinkPump running on Core %d", xPortGetCoreID());

    // Initialize cache
    m_cached_ws_state = sysdb.wsState();
    m_cached_pipeline_mode = sysdb.pipelineMode();

    while (m_running) {
        bool processed = processUplink();
        if (!processed) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

bool VoiceUplinkPump::processUplink() {
    auto& bm = BufferManager::getInstance();
    size_t chunk_size = 0;

    if (ulTaskNotifyTake(pdTRUE, 0) != 0) {
        m_cached_ws_state = sysdb.wsState();
        m_cached_pipeline_mode = sysdb.pipelineMode();
    }
    // While a session's connection is being made (or remade after a goAway),
    // leave the mic audio in MIC_TX_BUF (~4 s) and send it once setup is
    // complete: a handoff takes ~3 s and used to lose the start of the next
    // question. A closed or failed connection drains the buffer as before.
    if (m_cached_pipeline_mode == PipelineMode::GEMINI_LIVE &&
        WakeWordEngine::getInstance().isStreamingActive() &&
        (m_cached_ws_state == WsState::CONNECTING || m_cached_ws_state == WsState::CONNECTED) &&
        !VoiceAgent::active().ready()) {
        return false;
    }

    // Block on receive with a 100ms timeout
    uint8_t* pcm_data = static_cast<uint8_t*>(
        bm.receive(Buffers::MIC_TX_BUF, &chunk_size, pdMS_TO_TICKS(100), 1024));

    if (pcm_data != nullptr) {
        static int pump_read_count = 0;
        bool streaming = WakeWordEngine::getInstance().isStreamingActive();

        bool ws_connected = (m_cached_ws_state == WsState::CONNECTED);
        bool live_mode    = (m_cached_pipeline_mode == PipelineMode::GEMINI_LIVE);

        if (++pump_read_count % 200 == 1) {
            LOGI_AUDIO("AudioPump: chunk=%d bytes, streaming=%d, ws=%d, live_mode=%d",
                       (int)chunk_size, streaming ? 1 : 0, ws_connected ? 1 : 0, live_mode ? 1 : 0);
        }

        // Transmit only in GEMINI_LIVE mode, when streaming is active, and WebSocket is connected
        if (live_mode && streaming && ws_connected) {
            VoiceAgent::active().sendMicAudio(pcm_data, chunk_size);
        }

        // Always return the item
        bm.returnItem(Buffers::MIC_TX_BUF, pcm_data);
        return true;
    }
    return false;
}

