#pragma once

#include "common/ReactorTask.h"
#include "common/thread_config.h"
#include "common/hw_types.h"
#include "app/wake_word/IWakeWordListener.h"
#include "services/BufferManager.h"

class AudioHal;

/**
 * @brief Unified Audio Service — ReactorTask + IWakeWordListener.
 *
 * Replaces the old IService+TaskBase architecture.
 *
 * Watches:
 *   COMP::AUDIO    — volume, mic gain, sample rate, session flags
 *   COMP::PIPELINE — pipeline mode switches (GEMINI_LIVE ↔ RTP ↔ WAKE_IDLE)
 *
 * Owns:
 *   WakeWordEngine lifecycle (start/stop/pause/resume)
 *   AudioPipelineManager (static, controlled via SysDb mutations)
 *   AudioAlertPlayer async dispatch
 *
 * Injected:
 *   AudioHal& — hardware driver (I2S + codecs)
 */
class AudioService : public ReactorTask, public IWakeWordListener {
public:
    explicit AudioService(AudioHal& hal, const HardwareAudioHandles& handles);

    bool begin();

    // IWakeWordListener interface
    void onWakeWord(uint8_t channel) override;
    void onVadTimeout() override;
    void onUserSpeechDetected() override;
    void onSpeechDetected() override;

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    AudioHal&             m_hal;
    HardwareAudioHandles  m_handles;
    bool                  m_initialized = false;

    PipelineMode m_current_pipeline_mode  = PipelineMode::WAKE_IDLE;
    bool                  m_last_applied_mic_enabled = true;
    bool                  m_last_applied_session_active = false;

    void applyPipelineModeSwitch(PipelineMode mode);
    void enterAssistantPlaybackModeNow();
    void returnToWakeMode16k();

    static constexpr const char* TAG = "AudioSvc";
};
