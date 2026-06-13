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

    // Local shadow state — updated by onStateChanged(), applied by run()
    volatile int      m_pending_volume        = 80;
    volatile float    m_pending_gain           = 60.0f;
    volatile bool     m_pending_mic_enabled    = true;
    volatile uint32_t m_pending_sample_rate    = 16000;
    volatile bool     m_pending_pipeline_change = false;

    // Internal audio state (not cross-service — local to AudioService)
    uint32_t m_current_hardware_rate      = 16000;
    bool     m_dynamic_sr_enabled         = true;
    bool     m_session_active             = false;
    bool     m_assistant_speaking         = false;
    bool     m_turn_complete_pending      = false;
    uint64_t m_last_activity_ms           = 0;
    int      m_current_volume             = -1;
    float    m_current_gain               = -1.0f;
    PipelineMode m_current_pipeline_mode  = PipelineMode::WAKE_IDLE;

    void applyVolumeChange(int vol);
    void applyGainChange(float gain_db);
    void applySampleRateSwitch(uint32_t rate);
    void applyPipelineModeSwitch(PipelineMode mode);
    void enterAssistantPlaybackModeNow();
    void returnToWakeMode16k();
    void updateActivity();

    static constexpr const char* TAG = "AudioSvc";
};
