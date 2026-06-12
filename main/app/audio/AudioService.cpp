#include "AudioService.h"
#include "app/audio/AudioPipelineManager.h"
#include "app/audio/MicCapture.h"     // for Buffers::MIC_TX_BUF
#include "app/audio/SpeakerPlayback.h" // for Buffers::SPK_RX_BUF
#include "app/event/EventBus.h"
#include "app/wake_word/WakeWordDetector.h"
#include "common/AppLogger.h"
#include "common/events/app_events.h"
#include "app/assistant/AssistantEvents.h"
#include "hal/Board.h"

#include "services/BufferManager.h"
#include "esp_timer.h"

#if defined(CONFIG_VOICE_BACKEND_GEMINI_LIVE)
#include "app/gemini_live/GeminiLiveService.h"
#endif

AudioService &AudioService::getInstance() {
  static AudioService instance;
  return instance;
}

bool AudioService::begin(GlobalSystemSettings &settings,
                         HardwareAudioHandles  &handles,
                         Board                *board) {
  if (m_initialized)
    return true;

  m_settings = &settings;
  m_handles  = &handles;
  m_board    = board;

  return m_board ? onStart() : false;
}

bool AudioService::onStart() {
  // 1. Ensure board hardware is ready
  if (!m_board->isInitialized()) {
    if (!m_board->begin()) {
      LOGE_HAL("Failed to initialize physical board for AudioService.");
      return false;
    }
  }

  // 2. Map hardware handles from Board
  m_handles->speaker_tx_handle = m_board->getTxHandle();
  m_handles->mic_rx_handle     = m_board->getRxHandle();
  m_handles->play_dev          = m_board->getPlayDev();
  m_handles->record_dev        = m_board->getRecordDev();
  LOGI_HAL("Audio hardware handles mapped from Board.");

  // 3. Build audio pipeline (ring buffers come from BufferManager)
  if (!AudioPipelineManager::initialize(*m_settings, *m_handles)) {
    LOGE_AUDIO("Failed to build Audio Pipeline.");
    return false;
  }

  // 4. Wire WakeWordDetector — inject Board as feed source, self as listener
  auto &ww = WakeWordDetector::getInstance();
  ww.setFeedSource(m_board);  // Board implements IAudioFeedSource
  ww.setListener(this);       // AudioService implements IWakeWordListener

  // 5. Subscribe to EventBus events (MQTT-driven or server-driven state changes)
  subscribeEvent(APP_EVENTS, AppEvent::MIC_GAIN_UPDATE);
  subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::AUDIO_ENTER_CONVERSATION_MODE);
  subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::AUDIO_ENTER_PLAYBACK_MODE_24K);
  subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::AUDIO_RETURN_TO_WAKE_MODE_16K);
  subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::AUDIO_FLUSH_PLAYBACK);
  subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::AUDIO_RESUME_MIC_STREAMING);
  subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::AUDIO_SUSPEND_MIC_STREAMING);
  subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::ASSISTANT_TURN_COMPLETE);


  // 6. Start background supervisor task
  if (!this->start())
    return false;

  m_current_hardware_rate = m_settings->sample_rate;
  m_dynamic_sample_rate_enabled = m_settings->dynamic_sample_rate_enabled;
  m_initialized = true;
  LOGI_AUDIO("AudioService operational with rate %lu Hz.", (unsigned long)m_current_hardware_rate);
  return true;
}

bool AudioService::reinit(uint32_t sample_rate) {
  if (!m_initialized)
    return false;

  LOGI_AUDIO("AudioService: Reinitializing for sample rate %lu Hz", sample_rate);

  auto &ww            = WakeWordDetector::getInstance();
  bool  ww_was_running = ww.isRunning();
  if (ww_was_running) {
    ww.stop(); // semaphore-based — blocks until both tasks exit safely
  }

  AudioPipelineManager::teardown();
  m_board->reinitAudio(sample_rate);

  m_settings->sample_rate      = sample_rate;
  m_current_hardware_rate      = sample_rate;
  m_handles->speaker_tx_handle = m_board->getTxHandle();
  m_handles->mic_rx_handle     = m_board->getRxHandle();
  m_handles->play_dev          = m_board->getPlayDev();
  m_handles->record_dev        = m_board->getRecordDev();

  // Reallocate PSRAM buffers
  auto &bm = BufferManager::getInstance();
  bm.destroy(Buffers::MIC_TX_BUF);
  bm.destroy(Buffers::SPK_RX_BUF);
  bm.initAll();

  if (ww_was_running)
    ww.begin(); // re-inject feed source + listener already set

  return AudioPipelineManager::initialize(*m_settings, *m_handles);
}

void AudioService::setMicEnabled(bool enabled) {
  if (m_settings)
    m_settings->mic_enabled = enabled;

  auto &ww = WakeWordDetector::getInstance();
  if (ww.isRunning()) {
    AudioPipelineManager::setRtpEnabled(enabled);
  } else {
    AudioPipelineManager::setMicEnabled(enabled);
  }
}

void AudioService::setDynamicSampleRateEnabled(bool enabled) {
  m_dynamic_sample_rate_enabled = enabled;
  if (m_settings) {
    m_settings->dynamic_sample_rate_enabled = enabled;
  }
  LOGI_AUDIO("Dynamic sample-rate switching for assistant playback is now %s.",
             enabled ? "ENABLED" : "DISABLED");
}

void AudioService::enterAssistantPlaybackModeNow() {
  auto &ww = WakeWordDetector::getInstance();

  m_assistant_speaking = true;
  ww.setAssistantActive(true);
  ww.setVadDeferred(true);

  if (!m_dynamic_sample_rate_enabled) {
    LOGI_AUDIO("Immediate assistant playback path entered with dynamic sample-rate switching disabled. Staying at %lu Hz.",
               (unsigned long)m_current_hardware_rate);
    return;
  }

  if (m_current_hardware_rate == 24000) {
    return;
  }

  LOGI_AUDIO("Immediate assistant audio detected: switching hardware to 24kHz before queueing playback.");
  AudioPipelineManager::pauseSpeaker();
  ww.pauseHardware();
  Board::getInstance().setHardwareSampleRate(24000);
  m_current_hardware_rate = 24000;
  AudioPipelineManager::resumeSpeaker();
}

// ============================================================================
// IWakeWordListener callbacks — called from WakeWordDetector detectTask
// ============================================================================

void AudioService::onWakeWord(uint8_t channel) {
  LOGI_AUDIO("Wake word (ch %d) detected. Publishing WAKE_WORD_DETECTED.", channel);
  AssistantWakeWordData wd = {channel};
  EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::WAKE_WORD_DETECTED, wd);
}

void AudioService::onVadTimeout() {
  LOGI_AUDIO("VAD timeout. Publishing VAD_TIMEOUT.");
  EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::VAD_TIMEOUT, 0);
}

void AudioService::onUserSpeechDetected() {
  // Barge-in / User speech detected during assistant playback is ignored in stable half-duplex mode.
  LOGW_AUDIO("User speech detected during assistant playback; ignoring (Half-Duplex).");
}

// ============================================================================
// IService::onEvent — handles MQTT-driven / server-driven state changes
// ============================================================================

void AudioService::onEvent(esp_event_base_t base, int32_t id, void *data) {
  auto &ww = WakeWordDetector::getInstance();

  if (base == ASSISTANT_EVENTS) {
    switch (static_cast<AssistantEvent>(id)) {
      case AssistantEvent::AUDIO_ENTER_CONVERSATION_MODE: {
        LOGI_AUDIO("AudioService: Entering conversation mode");
        m_session_active = true;
        m_assistant_speaking = false;
        m_turn_complete_pending = false;
        updateActivity();

        if (m_current_hardware_rate != 16000) {
          LOGI_AUDIO("Conversation mode requires 16kHz capture. Restoring from %lu Hz.",
                     (unsigned long)m_current_hardware_rate);
          AudioPipelineManager::pauseSpeaker();
          Board::getInstance().setHardwareSampleRate(16000);
          m_current_hardware_rate = 16000;
          AudioPipelineManager::resumeSpeaker();
        }

        ww.setAssistantActive(false);
        ww.setVadDeferred(false);
        ww.resumeHardware();
        AudioPipelineManager::setRtpRxInterrupted(false);
        AudioPipelineManager::setRtpEnabled(true);
        m_board->setRecordGain(m_mic_gain);
        break;
      }
      case AssistantEvent::AUDIO_ENTER_PLAYBACK_MODE_24K: {
        enterAssistantPlaybackModeNow();
        break;
      }
      case AssistantEvent::AUDIO_RETURN_TO_WAKE_MODE_16K: {
        m_session_active = false;
        m_turn_complete_pending = false;
        m_assistant_speaking = false;

        ww.stopStreaming();

        if (m_current_hardware_rate == 16000) {
          LOGI_AUDIO("Streaming already stopped/idle at 16kHz. Bypassing redundant clock switches.");
        } else {
          LOGW_AUDIO("Streaming stop requested! Restoring safe idle 16kHz state...");

          // Reset physical I2S clock back to 16kHz safely
          AudioPipelineManager::pauseSpeaker();
          Board::getInstance().setHardwareSampleRate(16000);
          m_current_hardware_rate = 16000;

          ww.setAssistantActive(false);
          ww.setVadDeferred(false);
          ww.resumeHardware();
          AudioPipelineManager::resumeSpeaker();
        }
        AudioPipelineManager::setRtpRxInterrupted(false);

        // Reset buffer states
        BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);
        break;
      }
      case AssistantEvent::AUDIO_FLUSH_PLAYBACK: {
        BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);
        break;
      }
      case AssistantEvent::AUDIO_RESUME_MIC_STREAMING: {
        AudioPipelineManager::setRtpEnabled(true);
        break;
      }
      case AssistantEvent::AUDIO_SUSPEND_MIC_STREAMING: {
        AudioPipelineManager::setRtpEnabled(false);
        break;
      }
      case AssistantEvent::ASSISTANT_TURN_COMPLETE: {
        LOGI_AUDIO("Turn complete received from assistant state flow; pending final speaker drainage...");
        m_turn_complete_pending = true;
        break;
      }
      default:
        break;
    }
  } else if (base == APP_EVENTS) {
    switch (static_cast<AppEvent>(id)) {
      case AppEvent::MIC_GAIN_UPDATE:
        if (data) {
          m_mic_gain = *static_cast<float *>(data);
          LOGI_AUDIO("Stored optimal mic gain: %.1f dB", m_mic_gain);
        }
        break;
      default:
        break;
    }
  }
}

// ============================================================================
// Background supervisor task
// ============================================================================

void AudioService::updateActivity() {
  m_last_activity_ms = esp_timer_get_time() / 1000;
}

void AudioService::checkInactivityTimeout() {
  if (!m_session_active || m_assistant_speaking || m_turn_complete_pending) {
    return;
  }

  // If the speaker buffer still contains active audio samples being played back,
  // the assistant is actively talking. We update activity to prevent premature timeouts.
  if (BufferManager::getInstance().getUsedBytes(Buffers::SPK_RX_BUF) > 0) {
    updateActivity();
    return;
  }

  // Calculate silent elapsed duration
  uint64_t now_ms = esp_timer_get_time() / 1000;
  uint64_t inactive_ms = now_ms - m_last_activity_ms;

  if (inactive_ms >= 120000) { // 60-second silence timeout threshold for comfortable conversational pacing
    LOGW_AUDIO("Inactivity Timeout: 60 seconds of silence detected. Stopping session...");
    m_session_active = false;
    
    // Disable active audio streaming and re-arm WakeNet
    WakeWordDetector::getInstance().stopStreaming();

    // Reset speaker buffer and playback pipeline
    BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);

    EventBus::getInstance().publish(ASSISTANT_EVENTS, AssistantEvent::SESSION_IDLE_TIMEOUT, 0);
  }
}

void AudioService::run() {
  LOGI_AUDIO("AudioService background task active.");
  m_last_activity_ms = esp_timer_get_time() / 1000;
  uint32_t ticks = 0;
  while (m_running) {
    vTaskDelay(pdMS_TO_TICKS(50)); // Responsive 50ms supervisor interval

    if (m_turn_complete_pending) {
      size_t remaining_bytes = BufferManager::getInstance().getUsedBytes(Buffers::SPK_RX_BUF);
      if (remaining_bytes == 0) {
        m_turn_complete_pending = false;
        m_assistant_speaking = false;

        // Reset inactivity silence timer to start counting 10 seconds from exactly this moment
        updateActivity();

        if (m_current_hardware_rate == 16000) {
          LOGI_AUDIO("Speaker buffer drained but already running at 16kHz. Bypassing clock switch.");
        } else {
          LOGI_AUDIO("Speaker buffer fully drained. Safely switching clocks back to 16kHz...");
          auto &ww = WakeWordDetector::getInstance();
          AudioPipelineManager::pauseSpeaker();
          Board::getInstance().setHardwareSampleRate(16000);
          m_current_hardware_rate = 16000;

          ww.setAssistantActive(false);
          ww.setVadDeferred(false);
          ww.resumeHardware();
          AudioPipelineManager::resumeSpeaker();
          AudioPipelineManager::setRtpRxInterrupted(false);
        }

        AudioPipelineManager::setRtpEnabled(true);
      }
    }

    // Evaluate session timeout every 50ms
    // checkInactivityTimeout(); // DISABLED: Relying on pure WebSocket disconnection lifecycle per user request

    // Log PSRAM buffer health every 30 s (600 * 50ms = 30000ms)
    // if (++ticks >= 600) {
    //   BufferManager::getInstance().dumpStats();
    //   ticks = 0;
    // }
  }
}
