#pragma once

#include "common/app_types.h"
#include "esp_event.h"

// Declare the event base
ESP_EVENT_DECLARE_BASE(APP_EVENTS);

/**
 * @brief Singleton EventBus class for managing application-wide events
 */
class EventBus {
private:
  esp_event_loop_handle_t custom_loop;

  // Private constructor enforces Singleton pattern
  EventBus();

public:
  // Delete copy constructors
  EventBus(const EventBus &) = delete;
  void operator=(const EventBus &) = delete;

  static EventBus &getInstance();

  /**
   * @brief Initialize the event loop
   */
  void init();

  /**
   * @brief Post events to the loop
   * @tparam T Enum type for the ID
   * @tparam D Data type
   * @param base Event base
   * @param id Event ID (enum or enum class)
   * @param data Data reference
   */
  template <typename T, typename D>
  void publish(esp_event_base_t base, T id, const D &data) {
    esp_event_post_to(custom_loop, base, static_cast<int32_t>(id), &data,
                      sizeof(D), portMAX_DELAY);
  }

  /**
   * @brief Register a standard C-style function callback
   * @tparam T Enum type for the ID
   * @param base Event base
   * @param id Event ID (enum or enum class)
   * @param handler Callback function
   * @param arg User argument
   * @return esp_err_t
   */
  template <typename T>
  esp_err_t subscribe(esp_event_base_t base, T id, esp_event_handler_t handler,
                      void *arg) {
    return esp_event_handler_register_with(
        custom_loop, base, static_cast<int32_t>(id), handler, arg);
  }
};
