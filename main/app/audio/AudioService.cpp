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
  LOGI_AUDIO("Barge-in: flushing RX buffer");

  auto &ww = WakeWordDetector::getInstance();
  ww.setAssistantActive(false);
  ww.setVadDeferred(false);
  AudioPipelineManager::setRtpRxInterrupted(true);

  // One-liner drain — no manual while-loop
  BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);
  LOGI_AUDIO("Playback queue flushed.");

  uint32_t zero = 0;
  EventBus::getInstance().publish(APP_EVENTS, AppEvent::USER_INTERRUPTED, zero);
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
    LOGI_AUDIO("Assistant talking: deferring VAD");
    ww.setAssistantActive(true);
    ww.setVadDeferred(true);
    break;

  case AppEvent::ASSISTANT_SILENT:
    LOGI_AUDIO("Assistant silent: holding VAD defer");
    ww.setAssistantActive(true);
    ww.setVadDeferred(true);
    break;

  case AppEvent::ASSISTANT_TURN_COMPLETE:
    LOGI_AUDIO("Turn complete: re-arming VAD");
    ww.setAssistantActive(false);
    ww.setVadDeferred(false);
    AudioPipelineManager::setRtpRxInterrupted(false);
    break;

  default:
    break;
  }
}

// ============================================================================
// Background supervisor task
// ============================================================================

void AudioService::run() {
  LOGI_AUDIO("AudioService background task active.");
  uint32_t ticks = 0;
  while (m_running) {
    vTaskDelay(pdMS_TO_TICKS(500));
    // Log PSRAM buffer health every 30 s
    if (++ticks >= 60) {
      BufferManager::getInstance().dumpStats();
      ticks = 0;
    }
  }
}
