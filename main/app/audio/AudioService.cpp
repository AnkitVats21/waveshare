#include "AudioService.h"
#include "app/audio/AudioPipelineManager.h"
#include "common/AppLogger.h"
#include "hal/AudioHalManager.h"
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

  // 1. Initialize the Hardware HAL first
  if (!AudioHalManager::getInstance().begin(*m_settings, *m_handles, m_board)) {
    LOGE_HAL("Failed to initialize Audio HAL for AudioService.");
    return false;
  }

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

void AudioService::onSystemEvent(void *handler_arg, esp_event_base_t base,
                                 int32_t id, void *event_data) {
  if (base == APP_EVENTS) {
    if (id == (int32_t)AppEvent::WAKE_WORD_DETECTED) {
      LOGI_AUDIO("Wake Word event received. Ensuring Mic Gain is optimal...");
      AudioHalManager::getInstance().setMicGain(20.0f); // More reasonable gain
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
