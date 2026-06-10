#include "app/led/LedService.h"
#include "common/AppLogger.h"
#include "common/events/app_events.h"
#include "app/assistant/AssistantEvents.h"
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

// FIX: Pinned to Core 1, Priority 1. Keeps Core 0 isolated and fully dedicated to I2S DMA, Wi-Fi, and AFE.
LedService::LedService() : IService("LedSvc"), TaskBase({"LedService", 3072, 1, 1}) {
  m_current_command.mode        = LedMode::OFF;
  m_current_command.color       = OFF_LED;
  m_current_command.speed_ms    = 0;
  m_current_command.repeat_count = 0;

  // FIX: Explicitly initialize a FreeRTOS Mutex handle instead of standard library locks
  m_rtos_mutex = xSemaphoreCreateMutex();
}

// Clean up primitive allocation on teardown
LedService::~LedService() {
  if (m_rtos_mutex) {
    vSemaphoreDelete(m_rtos_mutex);
  }
}

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

  subscribeEvent(APP_EVENTS, AppEvent::LED_COMMAND);
  subscribeEvent(APP_EVENTS, AppEvent::LED_COLOR_UPDATE);
  subscribeEvent(ASSISTANT_EVENTS, AssistantEvent::VISUAL_STATE_CHANGED);

  if (!this->start()) {
    ESP_LOGE(TAG, "Failed to start LedService thread");
    return false;
  }

  m_initialized = true;
  ESP_LOGI(TAG, "LedService operational.");
  return true;
}

void LedService::onEvent(esp_event_base_t base, int32_t id, void *data) {
  if (base == ASSISTANT_EVENTS) {
    if (id == static_cast<int32_t>(AssistantEvent::VISUAL_STATE_CHANGED)) {
      if (data) {
        auto visual_state = *static_cast<AssistantVisualState *>(data);
        applyVisualState(visual_state);
      }
    }
  } else {
    applyEvent(id, data);
  }
}

void LedService::applyEvent(int32_t id, void *event_data) {
  // FIX: Protected safely with native RTOS binary structures
  if (m_rtos_mutex && xSemaphoreTake(m_rtos_mutex, portMAX_DELAY) == pdTRUE) {
    switch (static_cast<AppEvent>(id)) {
    case AppEvent::LED_COMMAND:
      if (event_data) {
        m_current_command = *static_cast<LedEventData *>(event_data);
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
      }
      break;

    default:
      break;
    }
    xSemaphoreGive(m_rtos_mutex);
  }
}

void LedService::applyVisualState(AssistantVisualState state) {
  if (m_rtos_mutex && xSemaphoreTake(m_rtos_mutex, portMAX_DELAY) == pdTRUE) {
    switch (state) {
      case AssistantVisualState::Idle:
        m_current_command = {LedMode::OFF, OFF_LED, 0, 0};
        ESP_LOGI(TAG, "LED State: Idle (OFF)");
        break;
      case AssistantVisualState::Listening:
        m_current_command = {LedMode::BREATH, GREEN_LED, 25, 0};
        ESP_LOGI(TAG, "LED State: Listening (Green Breathe)");
        break;
      case AssistantVisualState::Connecting:
        m_current_command = {LedMode::BLINK, BLUE_LED, 250, 0};
        ESP_LOGI(TAG, "LED State: Connecting (Blue Blink)");
        break;
      case AssistantVisualState::Speaking:
        m_current_command = {LedMode::RAINBOW, OFF_LED, 15, 0};
        ESP_LOGI(TAG, "LED State: Speaking (Rainbow Cycle)");
        break;
      case AssistantVisualState::Thinking:
        m_current_command = {LedMode::BREATH, BLUE_LED, 25, 0};
        ESP_LOGI(TAG, "LED State: Thinking (Blue Breathe)");
        break;
      case AssistantVisualState::Offline:
        m_current_command = {LedMode::SOLID, RED_LED, 0, 0};
        ESP_LOGI(TAG, "LED State: Offline (Red Solid)");
        break;
      case AssistantVisualState::Recovering:
        m_current_command = {LedMode::BLINK, ORANGE_LED, 300, 0};
        ESP_LOGI(TAG, "LED State: Recovering (Orange Blink)");
        break;
      case AssistantVisualState::RateLimited:
        m_current_command = {LedMode::BLINK, PURPLE_LED, 300, 0};
        ESP_LOGI(TAG, "LED State: Rate Limited (Purple Blink)");
        break;
      case AssistantVisualState::Error:
        m_current_command = {LedMode::BLINK, RED_LED, 250, 4};
        ESP_LOGI(TAG, "LED State: Error (Red Blink x 4)");
        break;
    }
    xSemaphoreGive(m_rtos_mutex);
  }
}

// ---------------------------------------------------------------------------
// Animation loop (TaskBase::run)
// ---------------------------------------------------------------------------
void LedService::run() {
  uint32_t step      = 0;
  bool     blink_state = false;
  float    breath_val  = 0.0f;
  bool     warned_not_ready = false;
  LedMode  last_mode   = LedMode::OFF;

  while (m_running) {
    if (!m_board || !m_board->isLedStripInitialized()) {
      if (!warned_not_ready) {
        ESP_LOGW(TAG, "LED strip is not initialized yet. Waiting...");
        warned_not_ready = true;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    warned_not_ready = false;

    LedEventData cmd;
    if (m_rtos_mutex && xSemaphoreTake(m_rtos_mutex, portMAX_DELAY) == pdTRUE) {
      cmd = m_current_command;
      xSemaphoreGive(m_rtos_mutex);
    }

    if (cmd.mode != last_mode) {
      step        = 0;
      blink_state = false;
      breath_val  = 0.0f;
      last_mode   = cmd.mode;
    }

    uint32_t delay_ms = 100;

    switch (cmd.mode) {
    case LedMode::OFF:
      m_board->clearLeds();
      delay_ms = 200;
      break;

    case LedMode::SOLID:
      m_board->setAllLedsColor(cmd.color.r, cmd.color.g, cmd.color.b);
      m_board->refreshLeds();
      delay_ms = 200;
      break;

    case LedMode::BLINK:
      if (cmd.repeat_count > 0 && step >= (uint32_t)cmd.repeat_count * 2) {
        m_board->clearLeds();
        if (m_rtos_mutex && xSemaphoreTake(m_rtos_mutex, portMAX_DELAY) == pdTRUE) {
          m_current_command.mode = LedMode::OFF;
          xSemaphoreGive(m_rtos_mutex);
        }
        last_mode = LedMode::OFF; // FIX: Keep state variables explicitly synced to prevent loop iteration skip flickering
        delay_ms = 200;
        break;
      }
      if (blink_state) {
        m_board->setAllLedsColor(cmd.color.r, cmd.color.g, cmd.color.b);
        m_board->refreshLeds();
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
      m_board->refreshLeds();

      breath_val += 0.08f;
      if (breath_val >= M_PI) {
        breath_val = 0.0f;
        if (cmd.repeat_count > 0 && ++step >= (uint32_t)cmd.repeat_count) {
          m_board->clearLeds();
          if (m_rtos_mutex && xSemaphoreTake(m_rtos_mutex, portMAX_DELAY) == pdTRUE) {
            m_current_command.mode = LedMode::OFF;
            xSemaphoreGive(m_rtos_mutex);
          }
          last_mode = LedMode::OFF; // FIX: Keep state variables explicitly synced to prevent loop iteration skip flickering
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
