#pragma once

#include "common/events/event_bases.h"
#include "esp_event.h"

/**
 * @brief Singleton EventBus — wraps a dedicated ESP-IDF custom event loop.
 *
 * Event bases are declared in common/events/event_bases.h.
 * Event IDs and payloads are in common/events/{app,wifi,mqtt}_events.h.
 *
 * Services should subscribe via IService::subscribeEvent() rather than
 * calling EventBus::subscribe() directly.
 */
class EventBus {
private:
  esp_event_loop_handle_t custom_loop = nullptr;

  EventBus();

public:
  EventBus(const EventBus &) = delete;
  void operator=(const EventBus &) = delete;

  static EventBus &getInstance();

  /** @brief Create the custom event loop task. Call once at startup. */
  void init();

  /**
   * @brief Publish an event with a typed payload.
   * @tparam T  Event ID enum type
   * @tparam D  Payload data type
   */
  template <typename T, typename D>
  void publish(esp_event_base_t base, T id, const D &data) {
    esp_event_post_to(custom_loop, base, static_cast<int32_t>(id), &data,
                      sizeof(D), portMAX_DELAY);
  }

  /**
   * @brief Register a C-style callback for an event.
   * @note  Prefer IService::subscribeEvent() — it routes to onEvent() automatically.
   */
  template <typename T>
  esp_err_t subscribe(esp_event_base_t base, T id,
                      esp_event_handler_t handler, void *arg) {
    return esp_event_handler_register_with(custom_loop, base,
                                           static_cast<int32_t>(id),
                                           handler, arg);
  }
};
