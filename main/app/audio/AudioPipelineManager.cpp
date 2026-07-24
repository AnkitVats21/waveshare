#include "sdkconfig.h"
#include "AudioPipelineManager.h"
#include "GeminiPCMDrainerTask.h"
#include "MicCapture.h"
#include "SpeakerPlayback.h"
#include "common/AppLogger.h"
#include "services/BufferManager.h"

SpeakerPlaybackTask*    AudioPipelineManager::m_speaker_task  = nullptr;
GeminiPCMDrainerTask*   AudioPipelineManager::m_drainer_task  = nullptr;
int                     AudioPipelineManager::m_shared_socket = -1;

bool AudioPipelineManager::initialize(uint32_t sample_rate,
                                      AudioHal& hal,
                                      const HardwareAudioHandles &hw_handles) {

    LOGI_AUDIO("Initializing Audio Pipeline (Sample Rate: %lu Hz)...", (unsigned long)sample_rate);

    // Verify ring buffers are allocated (done in app_main via BufferManager)
    if (!BufferManager::getInstance().handle(Buffers::MIC_TX_BUF) ||
        !BufferManager::getInstance().handle(Buffers::SPK_RX_BUF)) {
        LOGE_AUDIO("Ring buffers not allocated — was BufferManager initialized?");
        return false;
    }

    // 1. Start Hardware Tasks
#ifdef CONFIG_WAVESHARE_WAKEWORD_ENABLE
    // m_mic_task = new MicCaptureTask(hal);
    // m_mic_task->start(hw_handles.mic_rx_handle);
    // If WakeWordDetector is active, its feedTask takes exclusive ownership of
    // the mic. We soft-disable MicCaptureTask to prevent concurrent I2S reads
    // from corrupting the driver state. m_mic_task->setEnabled(false);
#endif

    m_speaker_task = new SpeakerPlaybackTask();
    m_speaker_task->start(hw_handles.play_dev);

    // Start the Gemini PCM drainer task — begins suspended (nothing in GEMINI_PCM_BUF yet)
    m_drainer_task = &GeminiPCMDrainerTask::getInstance();
    m_drainer_task->start();

    return true;
}

void AudioPipelineManager::teardown() {
    LOGI_AUDIO("Tearing down Audio Pipeline...");

    // 3. Stop hardware tasks
    // if (m_mic_task) {
    //     m_mic_task->stop();
    //     vTaskDelay(pdMS_TO_TICKS(50));
    //     delete m_mic_task;
    //     m_mic_task = nullptr;
    // }
    if (m_speaker_task) {
        m_speaker_task->stop();
        vTaskDelay(pdMS_TO_TICKS(50));
        delete m_speaker_task;
        m_speaker_task = nullptr;
    }
    if (m_drainer_task) {
        m_drainer_task->stop();
        m_drainer_task = nullptr;
    }

    // 4. Flush ring buffers (BufferManager owns them; do NOT delete)
    BufferManager::getInstance().flush(Buffers::MIC_TX_BUF);
    BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);
    BufferManager::getInstance().flush(Buffers::GEMINI_PCM_BUF);

    LOGI_AUDIO("Audio Pipeline teardown complete.");
}

void AudioPipelineManager::setMicEnabled(bool enabled) {
#ifndef CONFIG_WAVESHARE_WAKEWORD_ENABLE
    // if (m_mic_task) m_mic_task->setEnabled(enabled);
    // LOGI_AUDIO("Mic pipeline %s (Hardware Task)", enabled ? "ENABLED" : "SOFT-DISABLED");
#else
    (void)enabled;
    LOGI_AUDIO("Mic pipeline control bypassed (WakeWordEngine owns mic feed)");
#endif
}

void AudioPipelineManager::setRtpEnabled(bool enabled) {
    // Controlled via SysDb COMP_PIPELINE.rtp_enabled now
}

void AudioPipelineManager::setRtpRxInterrupted(bool interrupted) {
    // Optional: could be moved to RtpTransceiver or kept as a bypass
}

void AudioPipelineManager::pauseSpeaker() {
    if (m_speaker_task) {
        m_speaker_task->pauseHardware();
    }
}

void AudioPipelineManager::resumeSpeaker() {
    if (m_speaker_task) {
        m_speaker_task->resumeHardware();
    }
}

void AudioPipelineManager::suspendDrainer() {
    if (m_drainer_task) {
        m_drainer_task->suspendAndFlush();
    }
}

void AudioPipelineManager::resumeDrainer() {
    if (m_drainer_task) {
        m_drainer_task->resume();
    }
}

void AudioPipelineManager::setSpeakerPaused(bool paused) {
    if (m_speaker_task) {
        m_speaker_task->setPaused(paused);
    }
}

bool AudioPipelineManager::isSpeakerPaused() {
    if (m_speaker_task) {
        return m_speaker_task->isPaused();
    }
    return false;
}
