#include "AudioService.h"
#include "app/audio/AudioPipelineManager.h"
#include "common/AppLogger.h"
#include "hal/Board.h"
#include "services/EventBus.h"

AudioService &AudioService::getInstance() {
  static AudioService instance;
  return instance;
}

bool AudioService::begin(GlobalSystemSettings &settings,
                         GlobalPipelineContext &context,
                         HardwareAudioHandles &handles, Board *board,
                         EventBus *event_bus) {
  if (m_initialized)
    return true;

  m_settings = &settings;
  m_context = &context;
  m_handles = &handles;
  m_board = board;
  m_event_bus = event_bus;

  if (!m_board || !m_event_bus)
    return false;

  // 1. Initialize the Hardware handles from Board
  if (!m_board->isInitialized()) {
    if (!m_board->begin()) {
      LOGE_HAL("Failed to initialize physical board for AudioService.");
      return false;
    }
  }

  // Map low-level driver handles to our system context
  m_handles->speaker_tx_handle = m_board->getTxHandle();
  m_handles->mic_rx_handle = m_board->getRxHandle();
  m_handles->play_dev = m_board->getPlayDev();
  m_handles->record_dev = m_board->getRecordDev();

  LOGI_HAL("Audio hardware handles mapped directly from Board.");

  // 2. Build the Audio Pipeline (Pipes and RTP tasks)
  if (!AudioPipelineManager::initialize(*m_settings, *m_handles, *m_context)) {
    LOGE_AUDIO("Failed to build Audio Pipeline for AudioService.");
    return false;
  }

  // 3. Subscribe to relevant events
  m_event_bus->subscribe(APP_EVENTS, (int32_t)AppEvent::WAKE_WORD_DETECTED,
                         onSystemEvent, this);
  m_event_bus->subscribe(APP_EVENTS, (int32_t)AppEvent::STOP_STREAMING,
                         onSystemEvent, this);

  // 4. Start the background supervisor task
  if (!this->start()) {
    return false;
  }

  m_initialized = true;
  LOGI_AUDIO("AudioService operational (Unified Mic/Speaker Controller).");
  return true;
}

bool AudioService::reinit(uint32_t sample_rate) {
    if (!m_initialized) return false;
    
    LOGI_AUDIO("AudioService: Reinitializing for sample rate %lu Hz", sample_rate);
    
    // 1. Stop high-level tasks and pipeline
    AudioPipelineManager::teardown(*m_context);
    
    // 2. Reinit hardware via Board
    m_board->reinitAudio(sample_rate);
    
    // 3. Update settings
    m_settings->sample_rate = sample_rate;
    
    // 4. Update handles (they might have changed after reinit)
    m_handles->speaker_tx_handle = m_board->getTxHandle();
    m_handles->mic_rx_handle = m_board->getRxHandle();
    m_handles->play_dev = m_board->getPlayDev();
    m_handles->record_dev = m_board->getRecordDev();
    
    // 5. Restart pipeline
    return AudioPipelineManager::initialize(*m_settings, *m_handles, *m_context);
}

void AudioService::setMicEnabled(bool enabled) {
    if (m_settings) m_settings->mic_enabled = enabled;
    AudioPipelineManager::setMicEnabled(enabled);
}

void AudioService::onSystemEvent(void *handler_arg, esp_event_base_t base,
                                 int32_t id, void *event_data) {
  AudioService *self = static_cast<AudioService *>(handler_arg);
  if (!self) return;

  if (base == APP_EVENTS) {
    if (id == (int32_t)AppEvent::WAKE_WORD_DETECTED) {
      LOGI_AUDIO("Wake Word event received. Ensuring Mic Gain is optimal...");
      self->m_board->setRecordGain(20.0f); // More reasonable gain
    } else if (id == (int32_t)AppEvent::STOP_STREAMING) {
      LOGI_AUDIO("Streaming stop event. Resetting audio states.");
      // Reset volume or gain if needed
    }
  }
}

void AudioService::run() {
  LOGI_AUDIO("AudioService background task active.");

  while (m_running) {
    // Here we can implement:
    // 1. Peak level monitoring
    // 2. Slow volume fading
    // 3. Silence detection
    // 4. Resource monitoring

    vTaskDelay(pdMS_TO_TICKS(1000)); // Low frequency monitoring
  }
}
