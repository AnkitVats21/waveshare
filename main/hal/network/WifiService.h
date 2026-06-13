#pragma once

#include <string>
#include "esp_event.h"

/**
 * @brief Lightweight WiFi station driver that writes connectivity state
 *        directly into EmbeddedSysDb — no EventBus, no HalBase.
 *
 * Registers C-style handlers on the ESP-IDF default system event loop
 * for WIFI_EVENT and IP_EVENT.  When the IP address is assigned or
 * the connection is lost, it calls EmbeddedSysDb::mutate(COMP::SYSTEM)
 * so any ReactorTask that watches COMP::SYSTEM wakes up automatically.
 *
 * Lifecycle:
 *   1. Construct with Config (ssid, password, max_retries).
 *   2. Call begin() once — initialises NVS, netif, WiFi driver, registers
 *      event handlers and starts the station.
 *   3. No further interaction needed; SysDb drives everything from here.
 */
class WifiService {
public:
    struct Config {
        std::string ssid;
        std::string password;
        int         max_retries = 5;
    };

    explicit WifiService(const Config& cfg);

    /**
     * @brief Initialise NVS, netif, WiFi driver and begin connecting.
     * @return true on success.
     */
    bool begin();

private:
    Config m_config;
    int    m_retry_cnt = 0;

    // Static C-style handler — bridges ESP system events into SysDb
    static void sysEventHandler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data);

    static constexpr const char* TAG = "WifiService";
};
