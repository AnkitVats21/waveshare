#include "AudioService.h"
#include "app/audio/AudioPipelineManager.h"
#include "app/audio/MicCapture.h"     // for Buffers::MIC_TX_BUF
#include "app/audio/SpeakerPlayback.h" // for Buffers::SPK_RX_BUF
#include "app/event/EventBus.h"
#include "app/wake_word/WakeWordDetector.h"
#include "common/AppLogger.h"
#include "common/events/app_events.h"
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
  subscribeEvent(APP_EVENTS, AppEvent::ASSISTANT_TALKING);
  subscribeEvent(APP_EVENTS, AppEvent::ASSISTANT_SILENT);
  subscribeEvent(APP_EVENTS, AppEvent::ASSISTANT_TURN_COMPLETE);
  subscribeEvent(APP_EVENTS, AppEvent::STREAMING_STOP_REQUESTED);
  // Note: WAKE_WORD_DETECTED, STOP_STREAMING, USER_INTERRUPTED are now
  // handled via IWakeWordListener callbacks — no EventBus round-trip needed.

  // 6. Start background supervisor task
  if (!this->start())
    return false;

  m_initialized = true;
  LOGI_AUDIO("AudioService operational.");
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

// ============================================================================
// IWakeWordListener callbacks — called from WakeWordDetector detectTask
// ============================================================================

void AudioService::onWakeWord(uint8_t channel) {
  LOGI_AUDIO("Wake word (ch %d): enabling RTP, setting mic gain", channel);

  m_session_active = true;
  updateActivity();

  auto &ww = WakeWordDetector::getInstance();
  ww.setAssistantActive(false);
  ww.setVadDeferred(false);
  AudioPipelineManager::setRtpRxInterrupted(false);
  AudioPipelineManager::setRtpEnabled(true);
  m_board->setRecordGain(m_mic_gain);

  // Publish to EventBus so LedService and others can react
  WakeWordData wd = {channel};
  EventBus::getInstance().publish(APP_EVENTS, AppEvent::WAKE_WORD_DETECTED, wd);
}

void AudioService::onVadTimeout() {
  LOGI_AUDIO("VAD timeout: disabling RTP stream");

  auto &ww = WakeWordDetector::getInstance();
  ww.setAssistantActive(false);
  ww.setVadDeferred(false);
  AudioPipelineManager::setRtpEnabled(false);

  uint32_t zero = 0;
  EventBus::getInstance().publish(APP_EVENTS, AppEvent::STOP_STREAMING, zero);
}

void AudioService::onUserSpeechDetected() {
  LOGW_AUDIO("Barge-In Confirmed! Flushing playback channels and cutting cloud stream...");

  auto &ww = WakeWordDetector::getInstance();
  ww.setAssistantActive(false);
  ww.setVadDeferred(false);
  AudioPipelineManager::setRtpRxInterrupted(true);

  // 1. Immediately drop physical hardware playback to prevent echo loop pollution
  BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);

#if defined(CONFIG_VOICE_BACKEND_GEMINI_LIVE)
  // 2. Transmit structural cancellation state up the active WebSocket channel
  GeminiLiveService::getInstance().sendInterruptionSignal();
#endif

  // 3. Inform peripheral controllers (e.g. reset LED strip pattern to listening state)
  uint32_t payload_dummy = 0;
  EventBus::getInstance().publish(APP_EVENTS, AppEvent::USER_INTERRUPTED, payload_dummy);
}

// ============================================================================
// IService::onEvent — handles MQTT-driven / server-driven state changes
// ============================================================================

void AudioService::onEvent(esp_event_base_t /*base*/, int32_t id, void *data) {
  auto &ww = WakeWordDetector::getInstance();

  switch (static_cast<AppEvent>(id)) {

  case AppEvent::MIC_GAIN_UPDATE:
    if (data) {
      m_mic_gain = *static_cast<float *>(data);
      LOGI_AUDIO("Stored optimal mic gain: %.1f dB", m_mic_gain);
    }
    break;

  case AppEvent::ASSISTANT_TALKING:
    LOGI_AUDIO("Assistant talking: deferring VAD & switching clocks to 24kHz");
    ww.setAssistantActive(true);
    ww.setVadDeferred(true);
    
    // Pause both speaker playback task and wake word detector before switching clocks
    AudioPipelineManager::pauseSpeaker();
    ww.pauseHardware();

    // Reconfigure the clock speed
    Board::getInstance().setHardwareSampleRate(24000);

    // Resume speaker task safely at the new frequency
    AudioPipelineManager::resumeSpeaker();
    break;

  case AppEvent::ASSISTANT_SILENT:
    LOGI_AUDIO("Assistant silent: holding VAD defer");
    ww.setAssistantActive(true);
    ww.setVadDeferred(true);
    break;

  case AppEvent::ASSISTANT_TURN_COMPLETE:
    LOGI_AUDIO("Turn complete received from server; pending final speaker drainage...");
    m_turn_complete_pending = true;
    break;

  case AppEvent::STREAMING_STOP_REQUESTED:
    LOGW_AUDIO("Streaming stop requested (disconnection/error)! Restoring safe idle 16kHz state...");
    m_session_active = false;
    m_turn_complete_pending = false;

    // 1. Terminate active streaming and re-arm WakeNet
    ww.stopStreaming();

    // 2. Reset physical I2S clock back to 16kHz safely
    AudioPipelineManager::pauseSpeaker();
    Board::getInstance().setHardwareSampleRate(16000);
    ww.setAssistantActive(false);
    ww.setVadDeferred(false);
    ww.resumeHardware();
    AudioPipelineManager::resumeSpeaker();
    AudioPipelineManager::setRtpRxInterrupted(false);

    // 3. Reset buffer states
    BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);

    // 4. Propagate stop streaming to LedService and others
    {
      uint32_t zero = 0;
      EventBus::getInstance().publish(APP_EVENTS, AppEvent::STOP_STREAMING, zero);
    }
    break;

  default:
    break;
  }
}

// ============================================================================
// Background supervisor task
// ============================================================================

void AudioService::updateActivity() {
  m_last_activity_ms = esp_timer_get_time() / 1000;
}

void AudioService::checkInactivityTimeout() {
  if (!m_session_active) {
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

  if (inactive_ms >= 20000) { // 20-second silence timeout threshold for comfortable conversational pacing
    LOGW_AUDIO("Inactivity Timeout: 20 seconds of silence detected. Stopping session...");
    m_session_active = false;
    
    // Disable active audio streaming and re-arm WakeNet
    WakeWordDetector::getInstance().stopStreaming();

    // Reset speaker buffer and playback pipeline
    BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);

    // Stop streaming event publishes to restore idle peripheral modes (LEDs, etc)
    uint32_t zero = 0;
    EventBus::getInstance().publish(APP_EVENTS, AppEvent::STOP_STREAMING, zero);
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
        LOGI_AUDIO("Speaker buffer fully drained. Safely switching clocks back to 16kHz...");
        m_turn_complete_pending = false;

        auto &ww = WakeWordDetector::getInstance();
        AudioPipelineManager::pauseSpeaker();
        Board::getInstance().setHardwareSampleRate(16000);
        ww.setAssistantActive(false);
        ww.setVadDeferred(false);
        ww.resumeHardware();
        AudioPipelineManager::resumeSpeaker();
        AudioPipelineManager::setRtpRxInterrupted(false);
      }
    }

    // Evaluate session timeout every 50ms
    checkInactivityTimeout();

    // Log PSRAM buffer health every 30 s (600 * 50ms = 30000ms)
    if (++ticks >= 600) {
      BufferManager::getInstance().dumpStats();
      ticks = 0;
    }
  }
}
