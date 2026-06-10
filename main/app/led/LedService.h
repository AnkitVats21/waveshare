#pragma once

#include "common/IService.h"
#include "common/TaskBase.h"
#include "common/led_types.h"
#include "app/assistant/AssistantVisualState.h"

#include <mutex>

// Forward declarations to keep compilation fast
class Board;
class EventBus;

/**
 * @brief Low-priority LED animation and state orchestrator service.
 *
 * Extends IService for standardized event subscription.
 * Extends TaskBase for its own FreeRTOS animation loop.
 *
 * Handles continuous pattern loops (Rainbow, Breathing, Blinking) and solid
 * color transitions asynchronously without blocking other tasks.
 */
class LedService : public IService, public TaskBase {
public:
  static LedService &getInstance();

  // Disable copy and move semantics for strict Singleton pattern
  LedService(const LedService&) = delete;
  LedService& operator=(const LedService&) = delete;
  LedService(LedService&&) = delete;
  LedService& operator=(LedService&&) = delete;

  /**
   * @brief Initialize the service, subscribe to events, and start the thread.
   * @note Kept for backward compat with AppController::onStart().
   *       Internally calls onStart().
   */
  bool begin(Board *board, EventBus *event_bus);

  // IService interface
  bool onStart() override;
  void onEvent(esp_event_base_t base, int32_t id, void *data) override;

protected:
  void run() override; ///< TaskBase animation loop
  
  // Virtual destructor prevents undefined behavior during base pointer destruction
  ~LedService() override; 

private:
  LedService();

  void applyEvent(int32_t id, void *event_data);
  void applyVisualState(AssistantVisualState state);

  Board *m_board         = nullptr;
  bool   m_initialized   = false;

  std::mutex  m_mutex;
  LedEventData m_current_command{};
  SemaphoreHandle_t m_rtos_mutex;
  static constexpr const char *TAG = "LedSvc";
};
