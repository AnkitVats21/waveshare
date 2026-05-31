#include "app/led/LedService.h"
#include "common/AppLogger.h"
#include "common/events/app_events.h"
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

LedService::LedService() : IService("LedSvc"), TaskBase({"LedService", 3072, 2, 0}) {
  m_current_command.mode        = LedMode::OFF;
  m_current_command.color       = OFF_LED;
  m_current_command.speed_ms    = 0;
  m_current_command.repeat_count = 0;
}

// ---------------------------------------------------------------------------
// begin() — backward-compat shim used by AppController::onStart()
// ---------------------------------------------------------------------------
bool LedService::begin(Board *board, EventBus * /*event_bus*/) {
  m_board = board;
  return onStart();
}

bool LedService::onStart() {
  if (m_initialized)
    return true;

  if (!m_board) {
    ESP_LOGE(TAG, "Cannot start: board is null");
    return false;
  }

  // IService::subscribeEvent routes all these events to onEvent()
  subscribeEvent(APP_EVENTS, AppEvent::LED_COMMAND);
  subscribeEvent(APP_EVENTS, AppEvent::LED_COLOR_UPDATE);
  subscribeEvent(APP_EVENTS, AppEvent::WAKE_WORD_DETECTED);
  subscribeEvent(APP_EVENTS, AppEvent::STOP_STREAMING);

  if (!this->start()) {
    ESP_LOGE(TAG, "Failed to start LedService thread");
    return false;
  }

  m_initialized = true;
  ESP_LOGI(TAG, "LedService operational.");
  return true;
}

// ---------------------------------------------------------------------------
// IService::onEvent — single dispatch point, no manual cast needed
// ---------------------------------------------------------------------------
void LedService::onEvent(esp_event_base_t /*base*/, int32_t id, void *data) {
  applyEvent(id, data);
}

void LedService::applyEvent(int32_t id, void *event_data) {
  std::lock_guard<std::mutex> lock(m_mutex);

  switch (static_cast<AppEvent>(id)) {
  case AppEvent::LED_COMMAND:
    if (event_data) {
      m_current_command = *static_cast<LedEventData *>(event_data);
      m_command_dirty   = true;
      ESP_LOGD(TAG, "LedCommand updated: mode=%d", (int)m_current_command.mode);
    }
    break;

  case AppEvent::LED_COLOR_UPDATE:
    if (event_data) {
      RgbColor color         = *static_cast<RgbColor *>(event_data);
      m_current_command.mode = LedMode::SOLID;
      m_current_command.color = color;
      m_current_command.speed_ms    = 0;
      m_current_command.repeat_count = 0;
      m_command_dirty = true;
    }
    break;

  case AppEvent::WAKE_WORD_DETECTED:
    m_current_command = {LedMode::BREATH, {0, 100, 100}, 30, 0};
    m_command_dirty   = true;
    ESP_LOGI(TAG, "WakeWord: triggering BREATH animation");
    break;

  case AppEvent::STOP_STREAMING:
    m_current_command = {LedMode::OFF, OFF_LED, 0, 0};
    m_command_dirty   = true;
    ESP_LOGI(TAG, "StopStreaming: LEDs OFF");
    break;

  default:
    break;
  }
}

// ---------------------------------------------------------------------------
// Animation loop (TaskBase::run)
// ---------------------------------------------------------------------------
void LedService::run() {
  uint32_t step      = 0;
  bool     blink_state = false;
  float    breath_val  = 0.0f;

  while (m_running) {
    LedEventData cmd;
    bool dirty = false;

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      cmd   = m_current_command;
      dirty = m_command_dirty;
      m_command_dirty = false;
    }

    if (dirty) {
      step        = 0;
      blink_state = false;
      breath_val  = 0.0f;
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
      if (cmd.repeat_count > 0 && step >= (uint32_t)cmd.repeat_count * 2) {
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
      if (factor < 0) factor = -factor;

      m_board->setAllLedsColor(
          (uint8_t)(cmd.color.r * factor),
          (uint8_t)(cmd.color.g * factor),
          (uint8_t)(cmd.color.b * factor));

      breath_val += 0.08f;
      if (breath_val >= M_PI) {
        breath_val = 0.0f;
        if (cmd.repeat_count > 0 && ++step >= cmd.repeat_count) {
          m_board->clearLeds();
          std::lock_guard<std::mutex> lock(m_mutex);
          m_current_command.mode = LedMode::OFF;
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
          r = hue * 3; g = 255 - hue * 3; b = 0;
        } else if (hue < 170) {
          hue -= 85; r = 255 - hue * 3; g = 0; b = hue * 3;
        } else {
          hue -= 170; r = 0; g = hue * 3; b = 255 - hue * 3;
        }
        m_board->setLedPixel(i, r / 4, g / 4, b / 4);
      }
      m_board->refreshLeds();
      step      = (step + 5) & 255;
      delay_ms  = (cmd.speed_ms > 0) ? cmd.speed_ms : 30;
      break;
    }

    default:
      break;
    }

    vTaskDelay(pdMS_TO_TICKS(delay_ms));
  }
}
