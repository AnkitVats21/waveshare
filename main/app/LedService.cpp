#include "LedService.h"
#include "common/AppLogger.h"
#include "hal/Board.h"
#include "services/EventBus.h"
#include "freertos/task.h"

LedService &LedService::getInstance() {
  static LedService instance;
  return instance;
}

bool LedService::begin(Board* board, EventBus* event_bus) {
  if (m_initialized) return true;

  m_board = board;
  m_event_bus = event_bus;

  if (!m_board || !m_event_bus) return false;

  // Ensure hardware is ready
  if (!m_board->isInitialized()) {
      if (!m_board->begin()) {
          LOGE_HAL("Failed to initialize board for LedService.");
          return false;
      }
  }

  m_queue = xQueueCreate(10, sizeof(LedMode));
  if (!m_queue) return false;

  // Subscribe to Events
  m_event_bus->subscribe(WIFI_SYSTEM_EVENTS, WifiEvent::CONNECTED, onSystemEvent, this);
  m_event_bus->subscribe(WIFI_SYSTEM_EVENTS, WifiEvent::DISCONNECTED, onSystemEvent, this);
  m_event_bus->subscribe(APP_EVENTS, (int32_t)AppEvent::WAKE_WORD_DETECTED, onSystemEvent, this);
  m_event_bus->subscribe(APP_EVENTS, (int32_t)AppEvent::STOP_STREAMING, onSystemEvent, this);

  if (!this->start()) {
      return false;
  }

  m_initialized = true;
  LOGI_HAL("LedService operational (Integrated with EventBus).");
  return true;
}

void LedService::onSystemEvent(void *handler_arg, esp_event_base_t base, int32_t id, void *event_data) {
    LedService *self = static_cast<LedService *>(handler_arg);
    
    if (base == WIFI_SYSTEM_EVENTS) {
        if (id == (int32_t)WifiEvent::CONNECTED) {
            self->setMode(LED_MODE_IDLE);
        } else if (id == (int32_t)WifiEvent::DISCONNECTED) {
            self->setMode(LED_MODE_OFF);
        }
    } else if (base == APP_EVENTS) {
        if (id == (int32_t)AppEvent::WAKE_WORD_DETECTED) {
            self->setMode(LED_MODE_RECORDING);
        } else if (id == (int32_t)AppEvent::STOP_STREAMING) {
            self->setMode(LED_MODE_IDLE);
        }
    }
}

void LedService::setMode(LedMode mode) {
    if (m_queue) {
        xQueueSend(m_queue, &mode, pdMS_TO_TICKS(10));
    }
}

void LedService::run() {
    LedMode new_mode;
    bool reset_playing = false;

    while (m_running) {
        if (!m_board) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (xQueueReceive(m_queue, &new_mode, 0) == pdTRUE) {
            m_current_mode = new_mode;
            reset_playing = true;
            if (new_mode == LED_MODE_OFF || new_mode == LED_MODE_IDLE) {
                m_board->clearLeds();
            }
        }

        switch (m_current_mode) {
            case LED_MODE_PLAYING:
                patternPlaying(&reset_playing);
                break;
            case LED_MODE_RECORDING:
                patternRecording();
                break;
            default:
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
        }
    }
}

void LedService::patternPlaying(bool *reset) {
    static uint8_t color_val = 0;
    static int8_t step = 5;

    if (*reset) {
        color_val = 0;
        step = 5;
        *reset = false;
    }

    for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
        m_board->setLedPixel(i, 0, color_val, 0);
    }
    m_board->refreshLeds();

    color_val += step;
    if (color_val >= 150 || color_val <= 5) {
        step = -step;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
}

void LedService::patternRecording() {
    static int count = 0;
    static bool on = false;

    count++;
    if (count >= 10) {
        count = 0;
        on = !on;
        for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
            if (on) {
                m_board->setLedPixel(i, 150, 0, 0);
            } else {
                m_board->setLedPixel(i, 0, 0, 0);
            }
        }
        m_board->refreshLeds();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
}
