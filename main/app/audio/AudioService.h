#pragma once

#include "common/ReactorTask.h"
#include "common/hw_types.h"
#include "app/wake_word/IWakeWordListener.h"
#include <memory>

class AudioHal;
class SpeakerPlaybackTask;
class ICompanionAudioSink;
class ICompanionControl;

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
 *   SpeakerPlaybackTask lifecycle (mixing & I2S playback)
 *
 * Injected:
 *   AudioHal& — hardware driver (I2S + codecs)
 *   ICompanionAudioSink* — optional external Bluetooth companion audio sink
 *   ICompanionControl*   — optional external Bluetooth companion transport control
 */
class AudioService : public ReactorTask, public IWakeWordListener {
public:
    explicit AudioService(AudioHal& hal, const HardwareAudioHandles& handles,
                          ICompanionAudioSink* companion_sink = nullptr,
                          ICompanionControl* companion_ctrl = nullptr);
    ~AudioService() override;

    bool begin();

    // IWakeWordListener interface
    void onWakeWord(uint8_t channel) override;
    void onVadTimeout() override;
    void onUserSpeechDetected() override;
    void onSpeechDetected() override;

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:

private:
    AudioHal&             m_hal;
    HardwareAudioHandles  m_handles;
    bool                  m_initialized = false;

    PipelineMode m_current_pipeline_mode  = PipelineMode::WAKE_IDLE;
    bool                  m_last_applied_mic_enabled = true;
    bool                  m_last_applied_session_active = false;

    void applyPipelineModeSwitch(PipelineMode mode);
    void enterAssistantPlaybackModeNow();
    void returnToWakeMode();

    std::unique_ptr<SpeakerPlaybackTask> m_speaker_task;
    ICompanionAudioSink*                 m_companion_sink = nullptr;
    ICompanionControl*                   m_companion_ctrl = nullptr;

    static constexpr const char* TAG = "AudioSvc";
};
