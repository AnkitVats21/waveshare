#pragma once

#include "common/IService.h"

/**
 * @brief System orchestrator that reacts to network lifecycle events.
 *
 * Subscribes to WifiEvent::CONNECTED / DISCONNECTED and bootstraps or
 * tears down the audio and MQTT subsystems accordingly.
 *
 * Extends IService: subscriptions and dispatch are handled via the base class.
 * No raw pointers, settings refs, or handle refs are passed in — all come
 * from SystemContext::get().
 */
class AppController : public IService {
public:
  static AppController &getInstance();

  /**
   * @brief Subscribe to system events and start subsystems that don't
   *        need the network (e.g. LedService, WakeWordDetector).
   */
  bool onStart() override;

  /** @brief Convenience alias kept for backward compatibility with main.cpp. */
  void begin() { onStart(); }

  void onStop() override;

  void onEvent(esp_event_base_t base, int32_t id, void *data) override;

private:
  AppController() : IService("AppCtrl") {}
  ~AppController() = default;

  void bootstrapAudio();
  void teardownNetworkServices();
  void initMqtt();

  static constexpr const char *TAG = "AppCtrl";
};
