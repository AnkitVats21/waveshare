#pragma once

#include "common/TaskBase.h"
#include "common/app_types.h"
#include "esp_event.h"
#include <mutex>

class Board;
class EventBus;

/**
 * @brief Low-priority LED animation and state orchestrator service.
 * Handles continuous pattern loops (Rainbow, Breathing, Blinking)
 * and solid color transitions asynchronously without blocking other tasks.
 */
class LedService : public TaskBase {
public:
  static LedService &getInstance();

  /**
   * @brief Initialize the service, subscribe to events, and start the thread
   */
  bool begin(Board *board, EventBus *event_bus);

protected:
  /**
   * @brief Dynamic animation loop
   */
  void run() override;

private:
  LedService();
  ~LedService() = default;

  // Event handlers
  static void onSystemEvent(void *handler_arg, esp_event_base_t base,
                            int32_t id, void *event_data);

  void handleEvent(int32_t id, void *event_data);

  Board *m_board = nullptr;
  EventBus *m_event_bus = nullptr;
  bool m_initialized = false;

  std::mutex m_mutex;
  LedEventData m_current_command;
  bool m_command_dirty = false;

  static constexpr const char *TAG = "LedSvc";
};
