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

static auto &sysdb = EmbeddedSysDb::getInstance();

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

    auto snap = sysdb.snapshot();

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
    m_last_applied_session_active = snap.audio.session_active;
    
    // Sync initial hardware levels to match boot SysDb values
    m_hal.setPlayVolume(snap.audio.speaker_volume);
    m_hal.setRecordGain(snap.audio.mic_gain_db);
    AudioPipelineManager::setMicEnabled(snap.audio.mic_enabled);
    m_last_applied_mic_enabled = snap.audio.mic_enabled;

    LOGI_AUDIO("AudioService operational at %lu Hz.", (unsigned long)snap.audio.sample_rate);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReactorTask::onStateChanged — fast path, called from task context
// ─────────────────────────────────────────────────────────────────────────────

void AudioService::onStateChanged(ComponentMask changed, const SystemState& snap) {
    // 1. Reconcile Playback Volume
    if ((changed & BIT_AUDIO::SPEAKER_VOLUME) || (changed == 0)) {
        int target_vol = snap.audio.speaker_volume;
        if (m_hal.getPlayVolume() != target_vol) {
            m_hal.setPlayVolume(target_vol);
            LOGI_AUDIO("Volume applied: %d", target_vol);
        }
    }

    // 2. Reconcile Mic Record Gain
    if ((changed & BIT_AUDIO::MIC_GAIN) || (changed == 0)) {
        float target_gain = snap.audio.mic_gain_db;
        if (m_hal.getRecordGain() != target_gain) {
            m_hal.setRecordGain(target_gain);
            LOGI_AUDIO("Mic gain applied: %.1f dB", target_gain);
        }
    }

    // 3. Reconcile Mic Enablement
    if ((changed & BIT_AUDIO::MIC_ENABLED) || (changed == 0)) {
        if (snap.audio.mic_enabled != m_last_applied_mic_enabled) {
            AudioPipelineManager::setMicEnabled(snap.audio.mic_enabled);
            m_last_applied_mic_enabled = snap.audio.mic_enabled;
        }
    }

    // 4. Reconcile Sample Rate / Audio Mode Clock Switches (runs before pipeline mode to ensure hardware clock is ready)
    auto session = snap.assistant.session_state;
    bool session_changed = (changed & BIT_ASSISTANT::SESSION_STATE) || 
                           (changed & BIT_AUDIO::SESSION_ACTIVE) ||
                           (changed & BIT_AUDIO::ASST_SPEAKING) ||
                           (changed & BIT_AUDIO::TURN_COMPLETE) ||
                           (changed == 0);

    if (session_changed) {
        // Transition A: Enter 24kHz assistant speaking playback
        if (session == AssistantState::AssistantSpeaking && 
            !snap.audio.turn_complete_pending && 
            m_hal.getSampleRate() != 24000) {
            
            enterAssistantPlaybackModeNow();
        }

        // Transition B: Return to 16kHz wake mode when session becomes Idle/Closing
        bool session_ended = (m_last_applied_session_active && !snap.audio.session_active);
        bool needs_16k_restore = (session == AssistantState::Idle || session == AssistantState::Closing) &&
                                 (m_hal.getSampleRate() != 16000);

        if (session_ended || needs_16k_restore) {
            returnToWakeMode16k();
            m_last_applied_session_active = snap.audio.session_active;
        } else if (snap.audio.session_active != m_last_applied_session_active) {
            m_last_applied_session_active = snap.audio.session_active;
        }
    }

    // 5. Reconcile Pipeline Mode (runs after clock configuration)
    if ((changed & BIT_PIPELINE::MODE) || (changed == 0)) {
        if (snap.pipeline.mode != m_current_pipeline_mode) {
            applyPipelineModeSwitch(snap.pipeline.mode);
        }
    }

    // Transition C: Turn-complete cleanup (revert to 16kHz once speaker is empty)
    if ((changed & BIT_AUDIO::TURN_COMPLETE) && !snap.audio.turn_complete_pending) {
        // Revert to 16kHz clock
        if (m_hal.getSampleRate() != 16000) {
            AudioPipelineManager::pauseSpeaker();
            m_hal.setHardwareSampleRate(16000);
            AudioPipelineManager::resumeSpeaker();
        }
        
        auto& ww = WakeWordEngine::getInstance();
        ww.setAssistantActive(false);
        ww.setVadDeferred(false);
        ww.resumeHardware();
        AudioPipelineManager::setRtpRxInterrupted(false);
        
        sysdb.mutate([](SystemState& s) {
            s.pipeline.rtp_enabled = true;
        });
    }

    // Transition D: Reactive WAV format clock switching (e.g. for alarms)
    if ((changed & BIT_AUDIO::WAV_PLAYING) || (changed == 0)) {
        auto& ww = WakeWordEngine::getInstance();
        if (snap.audio.wav_playing) {
            // ONLY pause wake-word processing during WAV playback if it's NOT memory-prefetched
            if (!snap.audio.wav_prefetched) {
                ww.pauseHardware();
            } else {
                LOGI_AUDIO("WAV is memory prefetched. Keeping wake-word engine active during playback.");
            }
            if (m_hal.getSampleRate() != snap.audio.wav_sample_rate) {
                LOGI_AUDIO("Switching hardware to %lu Hz for WAV playback.", (unsigned long)snap.audio.wav_sample_rate);
                AudioPipelineManager::pauseSpeaker();
                m_hal.setHardwareSampleRate(snap.audio.wav_sample_rate);
                AudioPipelineManager::resumeSpeaker();
            }
        } else {
            if (m_hal.getSampleRate() != 16000) {
                LOGI_AUDIO("WAV playback finished. Restoring 16kHz idle clock...");
                AudioPipelineManager::pauseSpeaker();
                m_hal.setHardwareSampleRate(16000);
                AudioPipelineManager::resumeSpeaker();
            }
            // ALWAYS resume wake-word processing when WAV playback stops
            ww.resumeHardware();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IWakeWordListener — callbacks from WakeWordEngine::detectTask
// ─────────────────────────────────────────────────────────────────────────────

void AudioService::onWakeWord(uint8_t channel) {
    LOGI_AUDIO("Wake word (ch %d) — mutating SysDb COMP_ASSISTANT.", channel);
    sysdb.mutate([](SystemState& s) {
        s.assistant.session_state  = AssistantState::StartingSession;
        s.assistant.visual_state   = AssistantVisualState::Thinking;
        s.assistant.connect_requested = true;
    });
}

void AudioService::onVadTimeout() {
    LOGI_AUDIO("VAD timeout — returning to Idle.");
    sysdb.mutate([](SystemState& s) {
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
    
    auto snap = sysdb.snapshot();
    if (snap.pipeline.rtp_tx_en != tx || snap.pipeline.rtp_rx_en != rx) {
        sysdb.mutate([tx, rx](SystemState& s) {
            s.pipeline.rtp_tx_en = tx;
            s.pipeline.rtp_rx_en = rx;
        });
    }
}

void AudioService::enterAssistantPlaybackModeNow() {
    auto& ww = WakeWordEngine::getInstance();
    ww.setAssistantActive(true);
    ww.setVadDeferred(true);

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
        AudioPipelineManager::resumeSpeaker();
    }
    
    ww.setAssistantActive(false);
    ww.setVadDeferred(false);
    ww.resumeHardware();
    AudioPipelineManager::setRtpRxInterrupted(false);
    BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);
}
