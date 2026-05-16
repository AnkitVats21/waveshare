#pragma once

#include "common/app_types.h"
#include "hal/HalBase.h"
#include <string>

/**
 * @brief Manages the WiFi station connection and broadcasts events to the
 * system EventBus.
 *
 * Bridges low-level ESP-IDF WiFi/IP events into the application-level
 * WifiEvent enum via EventBus. Does not contain application-level policy.
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
   * @brief Initializes NVS, netif, and launches the WiFi station driver.
   * @return true if successful
   */
  bool begin() override;

private:
  Config m_config;
  int    m_retry_cnt;

  // System event handler — bridges ESP-IDF events to the application EventBus
  static void sysEventHandler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);

  static constexpr const char *TAG = "WIFI";
};
