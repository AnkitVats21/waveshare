#include "hal/network/WifiService.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/AppLogger.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include <cstring>

WifiService::WifiService(const Config& cfg) : m_config(cfg) {}

bool WifiService::begin() {
    // 1. NVS (required by the ESP WiFi hardware stack)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) return false;

    // 2. Network interface + default wifi station
    esp_netif_create_default_wifi_sta();

    // 3. Configure and init WiFi driver
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    // 4. Register ESP system event handlers (runs in system event loop task)
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiService::sysEventHandler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiService::sysEventHandler, this, nullptr));

    // 5. Build station config and start
    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid),
                 m_config.ssid.c_str(), sizeof(wifi_config.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password),
                 m_config.password.c_str(), sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Disable power-save for low-latency real-time audio streaming
    esp_wifi_set_ps(WIFI_PS_NONE);

    LOGI_WIFI("WiFi Station Driver initialised.");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Static ESP system event handler — bridges WiFi/IP events into SysDb
// ─────────────────────────────────────────────────────────────────────────────

void WifiService::sysEventHandler(void* arg, esp_event_base_t event_base,
                                  int32_t event_id, void* event_data) {
    WifiService* self = static_cast<WifiService*>(arg);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (self->m_retry_cnt < self->m_config.max_retries) {
            esp_wifi_connect();
            self->m_retry_cnt++;
            LOGI_WIFI("Retrying WiFi connection (%d/%d)...",
                      self->m_retry_cnt, self->m_config.max_retries);
        } else {
            LOGW_WIFI("Max retries reached — marking wifi_connected = false");
            // Write disconnected state directly into SysDb — no EventBus
            EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                s.system.wifi_connected = false;
            });
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = static_cast<ip_event_got_ip_t*>(event_data);
        LOGI_WIFI("Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        self->m_retry_cnt = 0;

        // Write connected state directly into SysDb — ReactorTasks wake automatically
        EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
            s.system.wifi_connected = true;
        });
    }
}
