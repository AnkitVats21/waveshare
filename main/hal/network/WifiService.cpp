#include "hal/network/WifiService.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/AppLogger.h"
#include "services/storage/StorageService.h"
#include "ArduinoJson.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <cstring>

WifiService::WifiService(const Config& cfg) : m_config(cfg) {}

bool WifiService::saveCredentials(const std::string& ssid, const std::string& password) {
    if (ssid.empty()) return false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_store", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_set_str(handle, "ssid", ssid.c_str());
        nvs_set_str(handle, "password", password.c_str());
        err = nvs_commit(handle);
        nvs_close(handle);
        if (err == ESP_OK) {
            LOGI_WIFI("Saved Wi-Fi credentials to NVS: SSID '%s'", ssid.c_str());
            return true;
        }
    }
    LOGE_WIFI("Failed to save Wi-Fi credentials to NVS: %s", esp_err_to_name(err));
    return false;
}

bool WifiService::clearStoredCredentials() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_store", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
        LOGI_WIFI("Cleared stored Wi-Fi credentials in NVS");
        return true;
    }
    return false;
}

bool WifiService::loadCredentials(std::string& outSsid, std::string& outPassword) {
    // 1. Tier 1: SD card JSON override (/sdcard/wifi_config.json)
    if (Services::StorageService::getInstance().isMounted() &&
        Services::StorageService::getInstance().fileExists("/sdcard/wifi_config.json")) {
        std::string content = Services::StorageService::getInstance().readFile("/sdcard/wifi_config.json");
        if (!content.empty()) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, content);
            if (!err && doc["ssid"].is<std::string>()) {
                outSsid = doc["ssid"].as<std::string>();
                outPassword = doc["password"] | "";
                if (!outSsid.empty()) {
                    LOGI_WIFI("Loaded Wi-Fi credentials from SD card (/sdcard/wifi_config.json): SSID '%s'", outSsid.c_str());
                    return true;
                }
            }
        }
    }

    // 2. Tier 2: NVS runtime store ("wifi_store")
    nvs_handle_t handle;
    if (nvs_open("wifi_store", NVS_READONLY, &handle) == ESP_OK) {
        char ssidBuf[65] = {0};
        char passBuf[65] = {0};
        size_t ssidLen = sizeof(ssidBuf);
        size_t passLen = sizeof(passBuf);

        if (nvs_get_str(handle, "ssid", ssidBuf, &ssidLen) == ESP_OK && ssidLen > 1) {
            outSsid = ssidBuf;
            if (nvs_get_str(handle, "password", passBuf, &passLen) == ESP_OK) {
                outPassword = passBuf;
            }
            nvs_close(handle);
            LOGI_WIFI("Loaded Wi-Fi credentials from NVS: SSID '%s'", outSsid.c_str());
            return true;
        }
        nvs_close(handle);
    }

    // 3. Tier 3: Compile-time Kconfig defaults
    if (!m_config.ssid.empty()) {
        outSsid = m_config.ssid;
        outPassword = m_config.password;
        LOGI_WIFI("Using fallback compile-time Wi-Fi credentials: SSID '%s'", outSsid.c_str());
        return true;
    }

    LOGW_WIFI("No Wi-Fi credentials found across SD, NVS, or Kconfig defaults");
    return false;
}

bool WifiService::connectWithCredentials(const std::string& ssid, const std::string& password) {
    if (ssid.empty()) return false;

    // Persist to NVS
    saveCredentials(ssid, password);

    m_config.ssid = ssid;
    m_config.password = password;
    m_retry_cnt = 0;

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid),
                 ssid.c_str(), sizeof(wifi_config.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password),
                 password.c_str(), sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
        s.system.network_state = NetworkState::Connecting;
        s.system.wifi_connected = false;
    });

    LOGI_WIFI("Reconnecting with new Wi-Fi credentials: SSID '%s'", ssid.c_str());
    return esp_wifi_connect() == ESP_OK;
}

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

    // 5. Resolve active credentials (SD -> NVS -> Kconfig)
    std::string activeSsid;
    std::string activePassword;
    loadCredentials(activeSsid, activePassword);
    m_config.ssid = activeSsid;
    m_config.password = activePassword;

    // 6. Build station config and start
    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid),
                 activeSsid.c_str(), sizeof(wifi_config.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password),
                 activePassword.c_str(), sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Disable power-save for low-latency real-time audio streaming
    esp_wifi_set_ps(WIFI_PS_NONE);

    LOGI_WIFI("WiFi Station Driver initialised (SSID: '%s').", activeSsid.c_str());
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Static ESP system event handler — bridges WiFi/IP events into SysDb
// ─────────────────────────────────────────────────────────────────────────────

void WifiService::sysEventHandler(void* arg, esp_event_base_t event_base,
                                  int32_t event_id, void* event_data) {
    WifiService* self = static_cast<WifiService*>(arg);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
            s.system.network_state = NetworkState::Connecting;
            s.system.wifi_connected = false;
        });
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (self->m_retry_cnt < self->m_config.max_retries) {
            EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                s.system.network_state = NetworkState::Connecting;
                s.system.wifi_connected = false;
            });
            esp_wifi_connect();
            self->m_retry_cnt++;
            LOGI_WIFI("Retrying WiFi connection (%d/%d)...",
                      self->m_retry_cnt, self->m_config.max_retries);
        } else {
            LOGW_WIFI("Max retries reached — marking network_state = Failed");
            EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                s.system.network_state = NetworkState::Failed;
                s.system.wifi_connected = false;
            });
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = static_cast<ip_event_got_ip_t*>(event_data);
        LOGI_WIFI("Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        self->m_retry_cnt = 0;

        EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
            s.system.network_state = NetworkState::Connected;
            s.system.wifi_connected = true;
        });
    }
}
