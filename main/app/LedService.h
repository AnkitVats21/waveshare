#pragma once

#include "hal/HalBase.h"
#include "common/TaskBase.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

class Board;
class EventBus;

/**
 * @brief LED Display Modes
 */
typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_IDLE,         // Ready state
    LED_MODE_PLAYING,      // Talking (Green Glow)
    LED_MODE_RECORDING,    // Listening (Red Blink)
} LedMode;

/**
 * @brief LedService coordinates animations and patterns.
 * High-level service that reacts to system events.
 */
class LedService : public HalBase, public TaskBase {
public:
  static LedService &getInstance();

  /**
   * @brief Initialize hardware and start the background task
   */
  bool begin(Board* board, EventBus* event_bus);

  bool begin() override { return false; }

  /**
   * @brief Manual override for the current animation mode
   */
  void setMode(LedMode mode);

protected:
  /**
   * @brief Background animation loop
   */
  void run() override;

private:
  LedService() : TaskBase({"LedService", 4096, 2, 0}) {}
  ~LedService() = default;
  
  // Event Handlers
  static void onSystemEvent(void *handler_arg, esp_event_base_t base, int32_t id, void *event_data);

  // Animation Patterns
  void patternPlaying(bool *reset);
  void patternRecording();

  Board *m_board = nullptr;
  EventBus *m_event_bus = nullptr;
  QueueHandle_t m_queue = nullptr;
  LedMode m_current_mode = LED_MODE_OFF;

  static constexpr const char *TAG = "LedSvc";
};
