#include "WifiManager.h"
#include "common/AppLogger.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "services/EventBus.h"
#include <cstring>

// Define the system-wide WiFi event base
ESP_EVENT_DEFINE_BASE(WIFI_SYSTEM_EVENTS);

WifiManager::WifiManager(const Config &config)
    : m_config(config), m_retry_cnt(0) {}

bool WifiManager::begin() {
  // 1. Initialize NVS (Required by the ESP WiFi hardware stack)
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  if (ret != ESP_OK)
    return false;

  // 2. Initialize Netif infrastructure and Default System Loop
  ESP_ERROR_CHECK(esp_netif_init());

  // Create default event loop if not already created
  esp_err_t loop_ret = esp_event_loop_create_default();
  if (loop_ret != ESP_OK && loop_ret != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(loop_ret);
  }

  esp_netif_create_default_wifi_sta();

  // 3. Configure and Initialize WiFi Driver
  wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

  // 4. Register class instance hooks with the internal system event loop
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiManager::sysEventHandler, this, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiManager::sysEventHandler, this,
      NULL));

  // 5. Build station structure and start hardware
  wifi_config_t wifi_config = {};
  std::strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid),
               m_config.ssid.c_str(), sizeof(wifi_config.sta.ssid));
  std::strncpy(reinterpret_cast<char *>(wifi_config.sta.password),
               m_config.password.c_str(), sizeof(wifi_config.sta.password));
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  // Disable WiFi Power Save mode to ensure low-latency for real-time audio
  esp_wifi_set_ps(WIFI_PS_NONE);

  LOGI_WIFI("WiFi Station Driver Initialized successfully.");
  m_initialized = true;
  return true;
}

void WifiManager::sysEventHandler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data) {
  WifiManager *instance = static_cast<WifiManager *>(arg);

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (instance->m_retry_cnt < instance->m_config.max_retry) {
      esp_wifi_connect();
      instance->m_retry_cnt++;
      LOGI_WIFI("Retrying connection to AP... (%d/%d)", instance->m_retry_cnt,
                instance->m_config.max_retry);
    } else {
      LOGW_WIFI("Failed to connect. Dispatching DISCONNECTED event.");
      uint8_t dummy = 0;
      EventBus::getInstance().publish(WIFI_SYSTEM_EVENTS,
                                      WifiEvent::DISCONNECTED, dummy);
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = static_cast<ip_event_got_ip_t *>(event_data);
    LOGI_WIFI("Successfully connected! Assigned IP: " IPSTR,
              IP2STR(&event->ip_info.ip));

    instance->m_retry_cnt = 0; // Reset counter
    uint8_t dummy = 1;
    EventBus::getInstance().publish(WIFI_SYSTEM_EVENTS, WifiEvent::CONNECTED,
                                    dummy);
  }
}
