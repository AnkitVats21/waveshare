#pragma once

#include "app/event/EventBus.h"
#include "esp_event.h"

/**
 * @brief Abstract base class for all application services.
 *
 * Provides a standardized lifecycle and a zero-boilerplate event subscription
 * mechanism. All concrete services (AudioService, LedService, MqttService, …)
 * extend this class.
 *
 * Lifecycle contract:
 *   1. Construct the singleton instance (private constructor enforced by Singleton).
 *   2. Call onStart() once at system startup — register event subscriptions here.
 *   3. Receive events via onEvent() — the dispatch boilerplate lives here, not
 *      in every derived class.
 *   4. Call onStop() on graceful shutdown (optional override).
 *
 * Usage:
 * @code
 *   class MyService : public IService {
 *   public:
 *     static MyService& getInstance() { static MyService i; return i; }
 *     bool onStart() override {
 *       subscribeEvent(APP_EVENTS, AppEvent::SOME_EVENT);
 *       return true;
 *     }
 *     void onEvent(esp_event_base_t, int32_t id, void* data) override {
 *       switch (static_cast<AppEvent>(id)) {
 *         case AppEvent::SOME_EVENT: handle(); break;
 *       }
 *     }
 *   private:
 *     MyService() : IService("MyService") {}
 *   };
 * @endcode
 */
class IService {
public:
  virtual ~IService() = default;

  /**
   * @brief Called once at startup. Register event subscriptions here.
   * @return false if the service could not initialize.
   */
  virtual bool onStart() = 0;

  /**
   * @brief Called on graceful system shutdown.
   */
  virtual void onStop() {}

  /**
   * @brief Invoked for every event this service has subscribed to.
   *
   * Default implementation is a no-op. Override to handle events.
   * All subscriptions made via subscribeEvent() route here automatically.
   */
  virtual void onEvent(esp_event_base_t /*base*/, int32_t /*id*/,
                       void * /*data*/) {}

  /** @brief Returns the service's human-readable name (for logging). */
  const char *name() const { return m_name; }

protected:
  explicit IService(const char *name) : m_name(name) {}

  /**
   * @brief Subscribe this service instance to an event.
   *
   * All events registered here will be routed to onEvent() automatically.
   * The static boilerplate cast lives once in eventDispatch(), not per class.
   *
   * @tparam T  Event ID enum type (e.g. AppEvent, WifiEvent)
   * @param base  ESP-IDF event base (e.g. APP_EVENTS, WIFI_SYSTEM_EVENTS)
   * @param id    Event ID value
   */
  template <typename T>
  void subscribeEvent(esp_event_base_t base, T id) {
    EventBus::getInstance().subscribe(base, id, &IService::eventDispatch, this);
  }

private:
  /**
   * @brief Single static dispatcher — casts arg to IService* and calls onEvent.
   *
   * This is the ONLY place the "cast handler_arg back to self" pattern lives.
   * Every service that extends IService gets it for free.
   */
  static void eventDispatch(void *arg, esp_event_base_t base, int32_t id,
                             void *event_data) {
    IService *self = static_cast<IService *>(arg);
    if (self) {
      self->onEvent(base, id, event_data);
    }
  }

  const char *m_name;
};
