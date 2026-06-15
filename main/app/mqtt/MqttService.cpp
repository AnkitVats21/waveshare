#include "MqttService.h"
#include "sdkconfig.h"
#include "common/AppLogger.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "mqtt_certificates.hpp"
#include <cstdint>
#include <sstream>

// MQTT configuration from Kconfig
static constexpr const char *MQTTS_BROKER_URI  = CONFIG_WAVESHARE_MQTT_BROKER_URI;
static constexpr const char *MQTT_USERNAME     = CONFIG_WAVESHARE_MQTT_USERNAME;
static constexpr const char *MQTT_PASSWORD     = CONFIG_WAVESHARE_MQTT_PASSWORD;
static constexpr const char *MQTT_CLIENT_ID    = CONFIG_WAVESHARE_MQTT_CLIENT_ID;
static constexpr const char *TOPIC_CONFIG      = "device/waveshare/config";
static constexpr const char *TOPIC_DYNAMIC_SUB = "device/subscribe/topic";

static auto &sysdb = EmbeddedSysDb::getInstance();

// ─────────────────────────────────────────────────────────────────────────────
// Construction & Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

MqttService& MqttService::getInstance() {
    static MqttService instance;
    return instance;
}

MqttService::MqttService()
    : ReactorTask({
          "mqtt_svc",
          ThreadConfig::StackSize::STACK_NORMAL,
          ThreadConfig::Priority::MQTT,
          ThreadConfig::CORE_NETWORK,
          COMP::SYSTEM | COMP::ASSISTANT
      })
{}

MqttService::~MqttService() {
    if (m_mqtt_handle) {
        esp_mqtt_client_stop(m_mqtt_handle);
        esp_mqtt_client_destroy(m_mqtt_handle);
    }
}

bool MqttService::begin() {
    LOGI_SYSTEM("MqttService operational.");
    return true;
}

bool MqttService::publish(const char* topic, const char* payload, int qos, int retain) {
    if (!m_mqtt_handle || !m_connected) {
        ESP_LOGW(TAG, "Cannot publish: MQTT client not connected/initialized");
        return false;
    }
    int msg_id = esp_mqtt_client_publish(m_mqtt_handle, topic, payload, 0, qos, retain);
    return msg_id >= 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReactorTask::onStateChanged
// ─────────────────────────────────────────────────────────────────────────────

void MqttService::onStateChanged(ComponentMask changed, const SystemState& snap) {
    if (changed & BIT_SYSTEM::WIFI_CONNECTED) {
        bool wifi_ok = snap.system.wifi_connected;

        // 1. React to WiFi Connectivity
        if (wifi_ok && !m_mqtt_handle) {
            LOGI_SYSTEM("WiFi connected — initializing MQTT client.");
            
            esp_mqtt_client_config_t mqtt_cfg = {};
            std::string broker_uri = MQTTS_BROKER_URI;
            if (broker_uri.find("mqtt://") != 0 && broker_uri.find("mqtts://") != 0 && 
                broker_uri.find("ws://") != 0 && broker_uri.find("wss://") != 0) {
                broker_uri = "mqtts://" + broker_uri;
            }
            mqtt_cfg.broker.address.uri = broker_uri.c_str();
            mqtt_cfg.broker.verification.certificate = BROKER_ROOT_CA_PEM;
            mqtt_cfg.credentials.username = MQTT_USERNAME;
            mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;
            mqtt_cfg.credentials.client_id = MQTT_CLIENT_ID;

            m_mqtt_handle = esp_mqtt_client_init(&mqtt_cfg);
            if (m_mqtt_handle) {
                esp_mqtt_client_register_event(m_mqtt_handle, MQTT_EVENT_ANY, mqttEventHandlerBridge, this);
                esp_mqtt_client_start(m_mqtt_handle);
            } else {
                ESP_LOGE(TAG, "Failed to initialize ESP-MQTT client.");
            }
        } 
        else if (!wifi_ok && m_mqtt_handle) {
            LOGW_SYSTEM("WiFi lost — stopping MQTT client.");
            esp_mqtt_client_stop(m_mqtt_handle);
            esp_mqtt_client_destroy(m_mqtt_handle);
            m_mqtt_handle = nullptr;
            m_connected = false;
            sysdb.mutate([](SystemState& s) {
                s.mqtt.connected = false;
            });
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT Events
// ─────────────────────────────────────────────────────────────────────────────

void MqttService::mqttEventHandlerBridge(void* handler_args, esp_event_base_t base,
                                         int32_t event_id, void* event_data) {
    auto self = static_cast<MqttService*>(handler_args);
    self->handleMqttEvent(event_id, static_cast<esp_mqtt_event_handle_t>(event_data));
}

void MqttService::handleMqttEvent(int32_t event_id, esp_mqtt_event_handle_t event) {
    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to MQTT broker securely!");
            m_connected = true;
            esp_mqtt_client_subscribe(m_mqtt_handle, "device/esp32s3/commands", 0);
            esp_mqtt_client_subscribe(m_mqtt_handle, TOPIC_CONFIG, 0);
            esp_mqtt_client_subscribe(m_mqtt_handle, TOPIC_DYNAMIC_SUB, 0);
            
            sysdb.mutate([](SystemState& s) {
                s.mqtt.connected = true;
            });
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from MQTT broker.");
            m_connected = false;
            
            sysdb.mutate([](SystemState& s) {
                s.mqtt.connected = false;
            });
            break;

        case MQTT_EVENT_DATA:
            processIncomingData(event);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTTS error occured");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_ESP_TLS) {
                ESP_LOGE(TAG, "TLS err: 0x%x, stack err: 0x%x",
                         event->error_handle->esp_tls_last_esp_err,
                         event->error_handle->esp_tls_stack_err);
            }
            break;

        default:
            break;
    }
}

void MqttService::processIncomingData(esp_mqtt_event_handle_t event) {
    std::string topic(event->topic, event->topic_len);
    std::string payload(event->data, event->data_len);

    if (topic == TOPIC_CONFIG) {
        LOGI_SYSTEM("Processing inbound MQTTS config...");
        std::stringstream ss(payload);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string val = line.substr(pos + 1);

                if (key == "speaker_volume" || key == "mic_volume" ||
                    key == "sample_rate" || key == "mic_enabled") {
                    handleAudioConfig(key, val);
                } else if (key == "led_color") {
                    handleLedConfig(val);
                }
            }
        }
    } else if (topic == TOPIC_DYNAMIC_SUB) {
        std::string new_topic = payload;
        // Trim whitespace
        new_topic.erase(0, new_topic.find_first_not_of(" \n\r\t"));
        new_topic.erase(new_topic.find_last_not_of(" \n\r\t") + 1);

        if (!new_topic.empty()) {
            int msg_id = esp_mqtt_client_subscribe(m_mqtt_handle, new_topic.c_str(), 0);
            if (msg_id >= 0) {
                ESP_LOGI(TAG, "Dynamically subscribed to %s", new_topic.c_str());
            }
        }
    }
}

void MqttService::handleAudioConfig(const std::string& key, const std::string& val) {
    try {
        if (key == "speaker_volume") {
            int vol = std::stoi(val);
            if (vol >= 0 && vol <= 100) {
                sysdb.mutate([vol](SystemState& s) {
                    s.audio.speaker_volume = vol;
                });
                ESP_LOGI(TAG, "Parsed config: speaker_volume = %d", vol);
            }
        } else if (key == "mic_volume") {
            float gain = std::stof(val);
            if (gain >= 0.0f && gain <= 100.0f) {
                sysdb.mutate([gain](SystemState& s) {
                    s.audio.mic_gain_db = gain;
                });
                ESP_LOGI(TAG, "Parsed config: mic_gain_db = %.1f dB", gain);
            }
        } else if (key == "sample_rate") {
            uint32_t sample_rate = std::stoi(val);
            sysdb.mutate([sample_rate](SystemState& s) {
                s.audio.sample_rate = sample_rate;
            });
            ESP_LOGI(TAG, "Parsed config: sample_rate = %d Hz", (int)sample_rate);
        } else if (key == "mic_enabled") {
            bool enabled = (val == "1" || val == "true");
            sysdb.mutate([enabled](SystemState& s) {
                s.audio.mic_enabled = enabled;
            });
            ESP_LOGI(TAG, "Parsed config: mic_enabled = %d", enabled);
        }
    } catch (...) {
        ESP_LOGE(TAG, "Failed to parse audio config: %s=%s", key.c_str(), val.c_str());
    }
}

void MqttService::handleLedConfig(const std::string& val) {
    int r, g, b;
    if (sscanf(val.c_str(), "%d,%d,%d", &r, &g, &b) == 3) {
        sysdb.mutate([r, g, b](SystemState& s) {
            s.led.mode = LedMode::SOLID;
            s.led.color = { (uint8_t)r, (uint8_t)g, (uint8_t)b };
        });
        ESP_LOGI(TAG, "Parsed config: led_color = %d,%d,%d", r, g, b);
    } else {
        ESP_LOGW(TAG, "Invalid color syntax: %s", val.c_str());
    }
}
