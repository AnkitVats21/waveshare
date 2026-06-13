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
    m_pending_volume     = snap.audio.speaker_volume;
    m_pending_gain       = snap.audio.mic_gain_db;
    m_current_hardware_rate = snap.audio.sample_rate;

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
    LOGI_AUDIO("AudioService operational at %lu Hz.", (unsigned long)m_current_hardware_rate);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReactorTask::onStateChanged — fast path, called from task context
// ─────────────────────────────────────────────────────────────────────────────

void AudioService::onStateChanged(ComponentMask changed, const SystemState& snap) {
    if (changed & COMP::AUDIO) {
        m_pending_volume = snap.audio.speaker_volume;
        m_pending_gain = snap.audio.mic_gain_db;
        m_pending_mic_enabled = snap.audio.mic_enabled;
        m_pending_sample_rate = snap.audio.sample_rate;
        m_session_active = snap.audio.session_active;
        m_assistant_speaking = snap.audio.assistant_speaking;
        m_turn_complete_pending = snap.audio.turn_complete_pending;
        updateActivity();
    }
    if (changed & COMP::PIPELINE) {
        m_pending_pipeline_change = true;
    }
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
    updateActivity();
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

void AudioService::applyVolumeChange(int vol) {
    if (m_current_volume == vol) return;
    m_hal.setPlayVolume(vol);
    m_current_volume = vol;
    LOGI_AUDIO("Volume applied: %d", vol);
}

void AudioService::applyGainChange(float gain_db) {
    if (m_current_gain == gain_db) return;
    m_hal.setRecordGain(gain_db);
    m_current_gain = gain_db;
    LOGI_AUDIO("Mic gain applied: %.1f dB", gain_db);
}

void AudioService::applySampleRateSwitch(uint32_t rate) {
    if (m_current_hardware_rate == rate) return;
    LOGI_AUDIO("Sample rate switching %lu → %lu Hz", (unsigned long)m_current_hardware_rate, (unsigned long)rate);
    AudioPipelineManager::pauseSpeaker();
    WakeWordEngine::getInstance().pauseHardware();
    m_hal.setHardwareSampleRate(rate);
    m_current_hardware_rate = rate;
    EmbeddedSysDb::getInstance().mutate(COMP::AUDIO, [rate](SystemState& s) {
        s.audio.current_hardware_rate = rate;
    });
    WakeWordEngine::getInstance().resumeHardware();
    AudioPipelineManager::resumeSpeaker();
}

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
    m_assistant_speaking = true;
    ww.setAssistantActive(true);
    ww.setVadDeferred(true);

    // Flush stale 16kHz RTP data from speaker buffer before switching sample rate
    BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);

    if (m_current_hardware_rate != 24000) {
        LOGI_AUDIO("Switching hardware to 24kHz for assistant playback.");
        AudioPipelineManager::pauseSpeaker();
        ww.pauseHardware();
        m_hal.setHardwareSampleRate(24000);
        m_current_hardware_rate = 24000;
        AudioPipelineManager::resumeSpeaker();
    }
}

void AudioService::returnToWakeMode16k() {
    auto& ww = WakeWordEngine::getInstance();
    m_session_active      = false;
    m_turn_complete_pending = false;
    m_assistant_speaking  = false;
    ww.stopStreaming();

    if (m_current_hardware_rate != 16000) {
        LOGW_AUDIO("Restoring 16kHz idle clock...");
        AudioPipelineManager::pauseSpeaker();
        m_hal.setHardwareSampleRate(16000);
        m_current_hardware_rate = 16000;
        ww.setAssistantActive(false);
        ww.setVadDeferred(false);
        ww.resumeHardware();
        AudioPipelineManager::resumeSpeaker();
    }
    AudioPipelineManager::setRtpRxInterrupted(false);
    BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);
}

void AudioService::updateActivity() {
    m_last_activity_ms = esp_timer_get_time() / 1000;
}

// ─────────────────────────────────────────────────────────────────────────────
// Background supervisor run() loop
// ─────────────────────────────────────────────────────────────────────────────

void AudioService::run() {
    LOGI_AUDIO("AudioService supervisor task active.");
    m_last_activity_ms = esp_timer_get_time() / 1000;

    while (m_running) {
        uint32_t changed_bits = 0;
        // Wait for a state change notification OR a 50ms supervisor tick
        BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &changed_bits, pdMS_TO_TICKS(50));
        if (!m_running) break;

        if (notified == pdTRUE && changed_bits > 0) {
            m_last_changed = changed_bits;
            SystemState snap = EmbeddedSysDb::getInstance().snapshot();
            onStateChanged(m_last_changed, snap);
        }

        // Take a fresh snapshot to read latest assistant state
        auto snap = EmbeddedSysDb::getInstance().snapshot();

        // Apply hardware changes from pending flags
        int   vol  = m_pending_volume;
        float gain = m_pending_gain;
        applyVolumeChange(vol);
        applyGainChange(gain);
        if (m_pending_mic_enabled != snap.audio.mic_enabled) {
            AudioPipelineManager::setMicEnabled(m_pending_mic_enabled);
        }
        if (m_pending_pipeline_change) {
            m_pending_pipeline_change = false;
            applyPipelineModeSwitch(snap.pipeline.mode);
        }

        // React to assistant state changes from SysDb
        auto session = snap.assistant.session_state;
        if (session == AssistantState::AssistantSpeaking && !m_assistant_speaking) {
            enterAssistantPlaybackModeNow();
        }
        if ((session == AssistantState::Idle || session == AssistantState::Closing)
            && (m_session_active || m_assistant_speaking)) {
            returnToWakeMode16k();
        }

        // Turn-complete buffer drain — switch back to 16kHz once speaker empties
        if (m_turn_complete_pending) {
            if (BufferManager::getInstance().getUsedBytes(Buffers::SPK_RX_BUF) == 0) {
                m_turn_complete_pending = false;
                m_assistant_speaking    = false;
                updateActivity();
                EmbeddedSysDb::getInstance().mutate(COMP::AUDIO, [](SystemState& s) {
                    s.audio.turn_complete_pending = false;
                    s.audio.assistant_speaking    = false;
                });
                if (m_current_hardware_rate != 16000) {
                    auto& ww = WakeWordEngine::getInstance();
                    AudioPipelineManager::pauseSpeaker();
                    m_hal.setHardwareSampleRate(16000);
                    m_current_hardware_rate = 16000;
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
