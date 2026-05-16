#pragma once

#include "common/app_types.h"
#include "esp_event.h"

/**
 * @brief System Orchestrator that coordinates between different modules
 * (WiFi, Audio, Logging) based on system events.
 */
class AppController {
public:
  static AppController &getInstance();

  /**
   * @brief Start the controller and subscribe to system events.
   */
  void begin(GlobalSystemSettings &settings, GlobalPipelineContext &context,
             HardwareAudioHandles &handles);

private:
  AppController() = default;
  ~AppController() = default;

  // Event Handlers
  static void onNetworkReady(void *handler_arg, esp_event_base_t base,
                             int32_t id, void *event_data);
  static void onNetworkLost(void *handler_arg, esp_event_base_t base,
                            int32_t id, void *event_data);

  // Internal Bootstrapping
  void bootstrapAudio();
  void teardownNetworkServices();
  void initMqtt();

  GlobalSystemSettings *m_settings = nullptr;
  GlobalPipelineContext *m_context = nullptr;
  HardwareAudioHandles *m_handles = nullptr;
  static constexpr const char *TAG = "AppCtrl";
};
