#include "AudioService.h"
#include "app/audio/AudioPipelineManager.h"
#include "app/audio/MicCapture.h"
#include "app/audio/SpeakerPlayback.h"
#include "app/audio/AudioAlertPlayer.h"
#include "app/wake_word/WakeWordEngine.h"
#include "common/AppLogger.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include "hal/audio/AudioHal.h"
#include "services/BufferManager.h"
#include "esp_timer.h"



// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

AudioService::AudioService(AudioHal& hal, const HardwareAudioHandles& handles)
    : ReactorTask({
          "audio_svc",
          ThreadConfig::StackSize::STACK_NORMAL,
          ThreadConfig::AUDIO_SERVICE,
          ThreadConfig::CORE_AUDIO,
          COMP::AUDIO | COMP::PIPELINE
      })
    , m_hal(hal)
    , m_handles(handles)
{}

// ─────────────────────────────────────────────────────────────────────────────
// Begin — called once after Board::begin()
// ─────────────────────────────────────────────────────────────────────────────

bool AudioService::begin() {
    if (m_initialized) return true;

    auto snap = EmbeddedSysDb::getInstance().snapshot();

    if (!AudioPipelineManager::initialize(snap.audio.sample_rate, m_hal, m_handles)) {
        LOGE_AUDIO("Failed to initialize AudioPipelineManager.");
        return false;
    }

    // Wire WakeWordEngine — inject AudioHal& as IAudioFeedSource, self as listener
    auto& ww = WakeWordEngine::getInstance();
    ww.setFeedSource(&m_hal);
    ww.setListener(this);

#ifdef CONFIG_WAVESHARE_WAKEWORD_ENABLE
    if (ww.begin()) {
        LOGI_AUDIO("WakeWordEngine started.");
    } else {
        LOGW_AUDIO("WakeWordEngine failed to start (check 'model' partition).");
    }
#endif

    m_initialized = true;
    LOGI_AUDIO("AudioService operational at %lu Hz.", (unsigned long)snap.audio.sample_rate);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReactorTask::onStateChanged — fast path, called from task context
// ─────────────────────────────────────────────────────────────────────────────

void AudioService::onStateChanged(ComponentMask changed, const SystemState& snap) {
    // Handled directly in run loop via xTaskNotifyWait & reconciliation
}

// ─────────────────────────────────────────────────────────────────────────────
// IWakeWordListener — callbacks from WakeWordEngine::detectTask
// ─────────────────────────────────────────────────────────────────────────────

void AudioService::onWakeWord(uint8_t channel) {
    LOGI_AUDIO("Wake word (ch %d) — mutating SysDb COMP_ASSISTANT.", channel);
    EmbeddedSysDb::getInstance().mutate(COMP::ASSISTANT, [](SystemState& s) {
        s.assistant.session_state  = AssistantState::StartingSession;
        s.assistant.visual_state   = AssistantVisualState::Thinking;
        s.assistant.connect_requested = true;
    });
}

void AudioService::onVadTimeout() {
    LOGI_AUDIO("VAD timeout — returning to Idle.");
    EmbeddedSysDb::getInstance().mutate(COMP::ASSISTANT, [](SystemState& s) {
        if (s.assistant.session_state == AssistantState::StreamingUserAudio ||
            s.assistant.session_state == AssistantState::WaitingForFollowup) {
            s.assistant.session_state = AssistantState::Idle;
            s.assistant.visual_state  = s.system.wifi_connected
                                        ? AssistantVisualState::Idle
                                        : AssistantVisualState::Offline;
        }
    });
}

void AudioService::onUserSpeechDetected() {
    LOGW_AUDIO("User speech during assistant playback — half-duplex, ignoring.");
}

void AudioService::onSpeechDetected() {
    // Handled
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

static const char* pipelineModeToString(PipelineMode mode) {
    switch (mode) {
        case PipelineMode::WAKE_IDLE:    return "WAKE_IDLE";
        case PipelineMode::GEMINI_LIVE:  return "GEMINI_LIVE";
        case PipelineMode::RTP_REMOTE:   return "RTP_REMOTE";
        case PipelineMode::RTP_WAKEWORD: return "RTP_WAKEWORD";
        default:                         return "Unknown";
    }
}

void AudioService::applyPipelineModeSwitch(PipelineMode mode) {
    if (m_current_pipeline_mode == mode && m_initialized) return;
    m_current_pipeline_mode = mode;

    LOGI_AUDIO("Pipeline mode switch → %s", pipelineModeToString(mode));
    bool tx = false;
    bool rx = false;
    switch (mode) {
        case PipelineMode::WAKE_IDLE:
            AudioPipelineManager::setRtpEnabled(false);
            WakeWordEngine::getInstance().resumeHardware();
            break;
        case PipelineMode::GEMINI_LIVE:
            // GeminiAudioPump handles its own uplink
            AudioPipelineManager::setRtpEnabled(false);
            break;
        case PipelineMode::RTP_REMOTE:
            AudioPipelineManager::setRtpEnabled(true);
            tx = true;
            rx = true;
            break;
        case PipelineMode::RTP_WAKEWORD:
            AudioPipelineManager::setRtpEnabled(true);
            tx = true;
            rx = true;
            break;
    }
    
    auto snap = EmbeddedSysDb::getInstance().snapshot();
    if (snap.pipeline.rtp_tx_en != tx || snap.pipeline.rtp_rx_en != rx) {
        EmbeddedSysDb::getInstance().mutate(COMP::PIPELINE, [tx, rx](SystemState& s) {
            s.pipeline.rtp_tx_en = tx;
            s.pipeline.rtp_rx_en = rx;
        });
    }
}

void AudioService::enterAssistantPlaybackModeNow() {
    auto& ww = WakeWordEngine::getInstance();
    ww.setAssistantActive(true);
    ww.setVadDeferred(true);

    // Flush stale 16kHz RTP data from speaker buffer before switching sample rate
    BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);

    if (m_hal.getSampleRate() != 24000) {
        LOGI_AUDIO("Switching hardware to 24kHz for assistant playback.");
        AudioPipelineManager::pauseSpeaker();
        ww.pauseHardware();
        m_hal.setHardwareSampleRate(24000);
        AudioPipelineManager::resumeSpeaker();
    }
}

void AudioService::returnToWakeMode16k() {
    auto& ww = WakeWordEngine::getInstance();
    ww.stopStreaming();

    if (m_hal.getSampleRate() != 16000) {
        LOGW_AUDIO("Restoring 16kHz idle clock...");
        AudioPipelineManager::pauseSpeaker();
        m_hal.setHardwareSampleRate(16000);
        ww.setAssistantActive(false);
        ww.setVadDeferred(false);
        ww.resumeHardware();
        AudioPipelineManager::resumeSpeaker();
    }
    AudioPipelineManager::setRtpRxInterrupted(false);
    BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);
}

// ─────────────────────────────────────────────────────────────────────────────
// Background supervisor run() loop
// ─────────────────────────────────────────────────────────────────────────────

void AudioService::run() {
    LOGI_AUDIO("AudioService supervisor task active.");

    while (m_running) {
        uint32_t changed_bits = 0;
        
        // Dynamic wait time: poll every 20ms ONLY when turn is complete & buffer is draining.
        // Otherwise, sleep indefinitely (portMAX_DELAY) until a state mutation notification occurs.
        auto snap = EmbeddedSysDb::getInstance().snapshot();
        TickType_t wait_ticks = snap.audio.turn_complete_pending ? pdMS_TO_TICKS(20) : portMAX_DELAY;

        BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &changed_bits, wait_ticks);
        (void)notified;
        if (!m_running) break;

        // Take a fresh snapshot to read latest assistant state
        snap = EmbeddedSysDb::getInstance().snapshot();

        // 1. Reconcile Playback Volume
        int target_vol = snap.audio.speaker_volume;
        if (m_hal.getPlayVolume() != target_vol) {
            m_hal.setPlayVolume(target_vol);
            LOGI_AUDIO("Volume applied: %d", target_vol);
        }

        // 2. Reconcile Mic Record Gain
        float target_gain = snap.audio.mic_gain_db;
        if (m_hal.getRecordGain() != target_gain) {
            m_hal.setRecordGain(target_gain);
            LOGI_AUDIO("Mic gain applied: %.1f dB", target_gain);
        }

        // 3. Reconcile Mic Enablement
        if (snap.audio.mic_enabled != m_last_applied_mic_enabled) {
            AudioPipelineManager::setMicEnabled(snap.audio.mic_enabled);
            m_last_applied_mic_enabled = snap.audio.mic_enabled;
        }

        // 4. Reconcile Pipeline Mode
        if (snap.pipeline.mode != m_current_pipeline_mode) {
            applyPipelineModeSwitch(snap.pipeline.mode);
        }

        // 5. Reconcile Sample Rate / Audio Mode Clock Switches
        auto session = snap.assistant.session_state;
        
        // Transition A: Enter 24kHz assistant speaking playback
        if (session == AssistantState::AssistantSpeaking && 
            !snap.audio.turn_complete_pending && 
            m_hal.getSampleRate() != 24000) {
            
            enterAssistantPlaybackModeNow();
        }

        // Transition B: Return to 16kHz wake mode when session becomes Idle/Closing
        if ((session == AssistantState::Idle || session == AssistantState::Closing) &&
            (snap.audio.session_active || m_hal.getSampleRate() != 16000)) {
            
            returnToWakeMode16k();
        }

        // Transition C: Turn-complete buffer drain (revert to 16kHz once speaker is empty)
        if (snap.audio.turn_complete_pending) {
            if (BufferManager::getInstance().getUsedBytes(Buffers::SPK_RX_BUF) == 0) {
                // Mutate DB to clear turn_complete_pending and assistant_speaking
                EmbeddedSysDb::getInstance().mutate(COMP::AUDIO, [](SystemState& s) {
                    s.audio.turn_complete_pending = false;
                    s.audio.assistant_speaking    = false;
                });
                
                // Revert to 16kHz clock
                if (m_hal.getSampleRate() != 16000) {
                    auto& ww = WakeWordEngine::getInstance();
                    AudioPipelineManager::pauseSpeaker();
                    m_hal.setHardwareSampleRate(16000);
                    ww.setAssistantActive(false);
                    ww.setVadDeferred(false);
                    ww.resumeHardware();
                    AudioPipelineManager::resumeSpeaker();
                    AudioPipelineManager::setRtpRxInterrupted(false);
                }
                
                EmbeddedSysDb::getInstance().mutate(COMP::PIPELINE, [](SystemState& s) {
                    s.pipeline.rtp_enabled = true;
                });
            }
        }
    }
}
