#include "hal/network/WifiService.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/AppLogger.h"
#include "common/thread_config.h"
#include "services/storage/StorageService.h"
#include "services/network/CaptiveDnsServer.h"
#include "ArduinoJson.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <cstring>

WifiService::WifiService(const Config& cfg)
    : ReactorTask(ReactorTask::Config{
          "WifiSvc",
          ThreadConfig::StackSize::STACK_NORMAL,
          ThreadConfig::Priority::LOW,
          ThreadConfig::CORE_NETWORK,
          COMP::SYSTEM
      }),
      m_config(cfg) {}

WifiService::~WifiService() {
    stopSoftAp();
}

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

    // 3. Tier 3: Optional compile-time Kconfig defaults (if defined)
    if (!m_config.ssid.empty()) {
        outSsid = m_config.ssid;
        outPassword = m_config.password;
        LOGI_WIFI("Using fallback compile-time Wi-Fi credentials: SSID '%s'", outSsid.c_str());
        return true;
    }

    LOGW_WIFI("No Wi-Fi credentials found across SD, NVS, or Kconfig defaults");
    return false;
}

bool WifiService::startSoftAp() {
    if (m_ap_active) return true;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ap_ssid[32];
    std::snprintf(ap_ssid, sizeof(ap_ssid), "Waveshare-Setup-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_config = {};
    std::strncpy(reinterpret_cast<char*>(ap_config.ap.ssid), ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    // Use APSTA mode so station scanning and background attempts can function
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STOPPED) {
        LOGE_WIFI("Failed to start SoftAP: %s", esp_err_to_name(err));
        return false;
    }

    m_ap_active = true;
    CaptiveDnsServer::getInstance().start();

    EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
        s.system.network_state = NetworkState::PortalActive;
        s.system.wifi_connected = false;
        s.system.ap_active = true;
    });

    LOGI_WIFI("SoftAP active: SSID '%s' (IP: 192.168.4.1, open auth)", ap_ssid);
    return true;
}

void WifiService::stopSoftAp() {
    if (!m_ap_active) return;

    CaptiveDnsServer::getInstance().stop();
    m_ap_active = false;

    esp_wifi_set_mode(WIFI_MODE_STA);

    EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
        s.system.ap_active = false;
    });

    LOGI_WIFI("SoftAP stopped, returned to pure STA mode");
}

bool WifiService::connectWithCredentials(const std::string& ssid, const std::string& password) {
    if (ssid.empty()) return false;

    // Persist to NVS so credentials survive subsequent reboots
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

    // Keep AP active (APSTA) while attempting to connect, so client can monitor progress
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    EmbeddedSysDb::getInstance().mutate([&ssid](SystemState& s) {
        s.system.network_state = NetworkState::Connecting;
        s.system.wifi_connected = false;
        std::strncpy(s.system.wifi_ssid, ssid.c_str(), sizeof(s.system.wifi_ssid) - 1);
        s.system.wifi_ssid[sizeof(s.system.wifi_ssid) - 1] = '\0';
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

    // 2. Network interfaces: Create default STA and AP netifs
    m_sta_netif = esp_netif_create_default_wifi_sta();
    m_ap_netif  = esp_netif_create_default_wifi_ap();

    // 3. Configure and init WiFi driver
    ESP_LOGI(TAG, "Free internal heap before Wi-Fi init: %u bytes, largest DMA block: %u bytes",
             (unsigned)esp_get_free_internal_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    init_cfg.static_rx_buf_num = 4;
    init_cfg.dynamic_rx_buf_num = 16;
    init_cfg.cache_tx_buf_num = 16;
    init_cfg.mgmt_sbuf_num = 16;
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    // 4. Register ESP system event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiService::sysEventHandler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiService::sysEventHandler, this, nullptr));

    // Disable power-save for low-latency real-time audio streaming
    esp_wifi_set_ps(WIFI_PS_NONE);

    // 5. Resolve active credentials (SD -> NVS -> Kconfig)
    std::string activeSsid;
    std::string activePassword;
    bool hasCreds = loadCredentials(activeSsid, activePassword);

    if (hasCreds && !activeSsid.empty()) {
        m_config.ssid = activeSsid;
        m_config.password = activePassword;
        m_retry_cnt = 0;

        wifi_config_t wifi_config = {};
        std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid),
                     activeSsid.c_str(), sizeof(wifi_config.sta.ssid));
        std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password),
                     activePassword.c_str(), sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        EmbeddedSysDb::getInstance().mutate([&activeSsid](SystemState& s) {
            s.system.network_state = NetworkState::Connecting;
            s.system.wifi_connected = false;
            s.system.ap_active = false;
            std::strncpy(s.system.wifi_ssid, activeSsid.c_str(), sizeof(s.system.wifi_ssid) - 1);
            s.system.wifi_ssid[sizeof(s.system.wifi_ssid) - 1] = '\0';
        });

        LOGI_WIFI("WiFi Station Driver initialised (SSID: '%s').", activeSsid.c_str());
    } else {
        LOGW_WIFI("No Wi-Fi credentials found across storage tiers. Starting SoftAP captive portal directly...");
        startSoftAp();
    }

    return true;
}

void WifiService::onStateChanged(ComponentMask changed, const SystemState& snap) {
    if (!(changed & COMP::SYSTEM)) return;

    if (changed & BIT_SYSTEM::APPLY_CREDS) {
        if (snap.system.wifi_apply_creds) {
            std::string ssid = snap.system.wifi_ssid;
            std::string pass = snap.system.wifi_password;

            // Clear trigger latch in SysDb
            EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                s.system.wifi_apply_creds = false;
            });

            if (!ssid.empty()) {
                LOGI_WIFI("SysDb triggered Wi-Fi connect request to SSID '%s'", ssid.c_str());
                connectWithCredentials(ssid, pass);
            }
        }
    }
}

void WifiService::run() {
    while (m_running) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!m_running) break;

        SystemState snap = EmbeddedSysDb::getInstance().snapshot();
        onStateChanged(COMP::SYSTEM, snap);
    }
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
            LOGW_WIFI("Max retries reached (%d) — falling back to SoftAP captive portal",
                      self->m_config.max_retries);
            self->startSoftAp();
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = static_cast<ip_event_got_ip_t*>(event_data);
        LOGI_WIFI("Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        self->m_retry_cnt = 0;

        // Disarm and close SoftAP now that STA connection is established
        self->stopSoftAp();

        EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
            s.system.network_state = NetworkState::Connected;
            s.system.wifi_connected = true;
            s.system.ap_active = false;
        });

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* evt = static_cast<wifi_event_ap_staconnected_t*>(event_data);
        LOGI_WIFI("Client connected to SoftAP: MAC " MACSTR " (AID: %d)",
                  MAC2STR(evt->mac), evt->aid);

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* evt = static_cast<wifi_event_ap_stadisconnected_t*>(event_data);
        LOGI_WIFI("Client disconnected from SoftAP: MAC " MACSTR " (AID: %d)",
                  MAC2STR(evt->mac), evt->aid);
    }
}
