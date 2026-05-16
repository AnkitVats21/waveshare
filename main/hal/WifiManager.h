#pragma once

#include "common/app_types.h"
#include "hal/HalBase.h"
#include <string>

/**
 * @brief Manages the WiFi connection and broadcasts events to the system
 * EventBus
 */
class WifiManager : public HalBase {
public:
  struct Config {
    std::string ssid;
    std::string password;
    int max_retry = 5;
  };

  WifiManager(const Config &config);

  /**
   * @brief Initializes NVS, network interfaces, and launches the WiFi driver
   * @return true if successful
   */
  bool begin() override;

private:
  Config m_config;
  int m_retry_cnt;
  static constexpr const char *TAG = "WifiManager";

  // System event handler (bridging hardware events to application EventBus)
  static void sysEventHandler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data);
};
