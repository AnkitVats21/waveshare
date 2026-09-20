#pragma once

#include "common/ReactorTask.h"
#include "esp_event.h"
#include "esp_netif.h"
#include <string>

/**
 * @brief Lightweight WiFi station and SoftAP driver with captive portal support.
 *
 * Automatically falls back to SoftAP mode when station credentials are missing
 * or when connection retries are exhausted.
 *
 * Operates as a ReactorTask to monitor SysDb for `BIT_SYSTEM::APPLY_CREDS` mutations,
 * allowing remote or local components to trigger seamless Wi-Fi reconfiguration.
 */
class WifiService : public ReactorTask {
public:
    struct Config {
        std::string ssid;
        std::string password;
        int         max_retries = 5;
    };

    explicit WifiService(const Config& cfg);
    ~WifiService() override;

    bool begin();

    // ReactorTask interface: react to BIT_SYSTEM::APPLY_CREDS
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

    /**
     * @brief Switch into SoftAP mode and launch captive portal DNS responder.
     */
    bool startSoftAp();

    /**
     * @brief Stop SoftAP mode and return to pure station mode.
     */
    void stopSoftAp();

    bool isApActive() const { return m_ap_active; }

    /**
     * @brief Load credentials following precedence: SD card -> NVS -> Kconfig.
     */
    bool loadCredentials(std::string& outSsid, std::string& outPassword);

    /**
     * @brief Save Wi-Fi credentials to NVS flash.
     */
    static bool saveCredentials(const std::string& ssid, const std::string& password);

    /**
     * @brief Clear stored Wi-Fi credentials in NVS.
     */
    static bool clearStoredCredentials();

    /**
     * @brief Reconfigure and connect using new credentials (e.g. from SoftAP portal).
     */
    bool connectWithCredentials(const std::string& ssid, const std::string& password);

    const Config& getConfig() const { return m_config; }

protected:
    void run() override;

private:
    WifiService(const WifiService&) = delete;
    WifiService& operator=(const WifiService&) = delete;

    Config       m_config;
    int          m_retry_cnt = 0;
    bool         m_ap_active = false;
    esp_netif_t* m_sta_netif = nullptr;
    esp_netif_t* m_ap_netif = nullptr;

    static void sysEventHandler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data);

    static constexpr const char* TAG = "WifiService";
};
