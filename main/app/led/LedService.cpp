#include "app/led/LedService.h"
#include "app/event/EventBus.h"
#include "common/AppLogger.h"
#include "esp_log.h"
#include "hal/Board.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LedService &LedService::getInstance() {
  static LedService instance;
  return instance;
}

LedService::LedService() : TaskBase({"LedService", 3072, 2, 1}) {
  m_current_command.mode = LedMode::OFF;
  m_current_command.color = {0, 0, 0};
  m_current_command.speed_ms = 0;
  m_current_command.repeat_count = 0;
}

bool LedService::begin(Board *board, EventBus *event_bus) {
  if (m_initialized)
    return true;

  m_board = board;
  m_event_bus = event_bus;

  if (!m_board || !m_event_bus) {
    ESP_LOGE(TAG, "Cannot start: board or event_bus is null");
    return false;
  }

  // Subscribe to LED and system notifications
  m_event_bus->subscribe(APP_EVENTS, (int32_t)AppEvent::LED_COMMAND,
                         onSystemEvent, this);
  m_event_bus->subscribe(APP_EVENTS, (int32_t)AppEvent::LED_COLOR_UPDATE,
                         onSystemEvent, this);
  m_event_bus->subscribe(APP_EVENTS, (int32_t)AppEvent::WAKE_WORD_DETECTED,
                         onSystemEvent, this);
  m_event_bus->subscribe(APP_EVENTS, (int32_t)AppEvent::STOP_STREAMING,
                         onSystemEvent, this);

  if (!this->start()) {
    ESP_LOGE(TAG, "Failed to start LedService thread");
    return false;
  }

  m_initialized = true;
  ESP_LOGI(TAG, "LedService operational.");
  return true;
}

void LedService::onSystemEvent(void *handler_arg, esp_event_base_t base,
                               int32_t id, void *event_data) {
  LedService *self = static_cast<LedService *>(handler_arg);
  if (self) {
    self->handleEvent(id, event_data);
  }
}

void LedService::handleEvent(int32_t id, void *event_data) {
  std::lock_guard<std::mutex> lock(m_mutex);

  switch (static_cast<AppEvent>(id)) {
  case AppEvent::LED_COMMAND:
    if (event_data) {
      m_current_command = *static_cast<LedEventData *>(event_data);
      m_command_dirty = true;
      ESP_LOGD(TAG, "LedCommand updated: mode=%d, r=%d, g=%d, b=%d",
               (int)m_current_command.mode, m_current_command.color.r,
               m_current_command.color.g, m_current_command.color.b);
    }
    break;

  case AppEvent::LED_COLOR_UPDATE:
    if (event_data) {
      RgbColor color = *static_cast<RgbColor *>(event_data);
      m_current_command.mode = LedMode::SOLID;
      m_current_command.color = color;
      m_current_command.speed_ms = 0;
      m_current_command.repeat_count = 0;
      m_command_dirty = true;
      ESP_LOGI(TAG, "LedColorUpdate: SOLID r=%d, g=%d, b=%d", color.r, color.g,
               color.b);
    }
    break;

  case AppEvent::WAKE_WORD_DETECTED:
    // When wake word is detected, let's start a beautiful continuous rainbow
    // pattern or pulsing breathing effect! Since the original was
    // setAllLedsColor(0, 100, 100)[g,r,b] (cyan/magenta), let's breathing pulse
    // that color!
    m_current_command.mode = LedMode::BREATH;
    m_current_command.color = {0, 100, 100};
    m_current_command.speed_ms = 30; // smooth breath timing
    m_current_command.repeat_count = 0;
    m_command_dirty = true;
    ESP_LOGI(TAG, "WakeWord detected event: Triggering BREATH pink animation");
    break;

  case AppEvent::STOP_STREAMING:
    // When we stop streaming, turn the LEDs off
    m_current_command.mode = LedMode::OFF;
    m_current_command.color = {0, 0, 0};
    m_current_command.speed_ms = 0;
    m_current_command.repeat_count = 0;
    m_command_dirty = true;
    ESP_LOGI(TAG, "StopStreaming event: Turning LEDs OFF");
    break;

  default:
    break;
  }
}

void LedService::run() {
  uint32_t step = 0;
  bool blink_state = false;
  float breath_val = 0.0f;
  float breath_dir = 0.08f; // breathing speed

  while (m_running) {
    LedEventData cmd;
    bool dirty = false;

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      cmd = m_current_command;
      dirty = m_command_dirty;
      m_command_dirty = false;
    }

    if (dirty) {
      step = 0;
      blink_state = false;
      breath_val = 0.0f;
    }

    uint32_t delay_ms = 100;

    switch (cmd.mode) {
    case LedMode::OFF:
      m_board->clearLeds();
      delay_ms = 200;
      break;

    case LedMode::SOLID:
      m_board->setAllLedsColor(cmd.color.r, cmd.color.g, cmd.color.b);
      delay_ms = 200;
      break;

    case LedMode::BLINK:
      if (cmd.repeat_count > 0 && step >= cmd.repeat_count * 2) {
        // Blink finished, turn off
        m_board->clearLeds();
        {
          std::lock_guard<std::mutex> lock(m_mutex);
          m_current_command.mode = LedMode::OFF;
        }
        delay_ms = 200;
        break;
      }
      if (blink_state) {
        m_board->setAllLedsColor(cmd.color.r, cmd.color.g, cmd.color.b);
      } else {
        m_board->clearLeds();
      }
      blink_state = !blink_state;
      step++;
      delay_ms = (cmd.speed_ms > 0) ? cmd.speed_ms : 500;
      break;

    case LedMode::BREATH: {
      float factor = sinf(breath_val);
      if (factor < 0)
        factor = -factor;

      uint8_t r = (uint8_t)(cmd.color.r * factor);
      uint8_t g = (uint8_t)(cmd.color.g * factor);
      uint8_t b = (uint8_t)(cmd.color.b * factor);

      m_board->setAllLedsColor(r, g, b);

      breath_val += breath_dir;
      if (breath_val >= M_PI) {
        breath_val = 0.0f;
        if (cmd.repeat_count > 0) {
          step++;
          if (step >= cmd.repeat_count) {
            m_board->clearLeds();
            {
              std::lock_guard<std::mutex> lock(m_mutex);
              m_current_command.mode = LedMode::OFF;
            }
          }
        }
      }
      delay_ms = (cmd.speed_ms > 0) ? cmd.speed_ms : 30;
      break;
    }

    case LedMode::RAINBOW: {
      for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
        uint8_t hue = (step + (i * 256 / LED_STRIP_LED_COUNT)) & 255;
        uint8_t r = 0, g = 0, b = 0;

        if (hue < 85) {
          r = hue * 3;
          g = 255 - hue * 3;
          b = 0;
        } else if (hue < 170) {
          hue -= 85;
          r = 255 - hue * 3;
          g = 0;
          b = hue * 3;
        } else {
          hue -= 170;
          r = 0;
          g = hue * 3;
          b = 255 - hue * 3;
        }

        // Scale brightness down to 25% (very bright WS2812 can draw a lot of
        // power)
        m_board->setLedPixel(i, r / 4, g / 4, b / 4);
      }
      m_board->refreshLeds();
      step = (step + 5) & 255;
      delay_ms = (cmd.speed_ms > 0) ? cmd.speed_ms : 30;
      break;
    }

    default:
      break;
    }

    vTaskDelay(pdMS_TO_TICKS(delay_ms));
  }
}
