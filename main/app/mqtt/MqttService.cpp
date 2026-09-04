#include "MqttService.h"
#include "sdkconfig.h"
#include "common/AppLogger.h"
#include "common/ParserUtils.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "mqtt_certificates.hpp"
#include "services/storage/StorageService.h"
#include "services/config_manager/config.h"
#include <cstdint>
#include <sstream>
#include <ArduinoJson.h>
#include "app/media_player/NexusPlayer.h"
#include "app/media_player/MusicPlaybackService.h"

// MQTT configuration from Kconfig with fallback defaults
#ifdef CONFIG_WAVESHARE_MQTT_BROKER_URI
static constexpr const char *MQTTS_BROKER_URI  = CONFIG_WAVESHARE_MQTT_BROKER_URI;
static constexpr const char *MQTT_USERNAME     = CONFIG_WAVESHARE_MQTT_USERNAME;
static constexpr const char *MQTT_PASSWORD     = CONFIG_WAVESHARE_MQTT_PASSWORD;
static constexpr const char *MQTT_CLIENT_ID    = CONFIG_WAVESHARE_MQTT_CLIENT_ID;
#else
static constexpr const char *MQTTS_BROKER_URI  = "mqtt://broker.emqx.io";
static constexpr const char *MQTT_USERNAME     = "";
static constexpr const char *MQTT_PASSWORD     = "";
static constexpr const char *MQTT_CLIENT_ID    = "waveshare-esp32";
#endif
static constexpr const char *TOPIC_CONFIG      = "device/waveshare/config";
static constexpr const char *TOPIC_DYNAMIC_SUB = "device/subscribe/topic";
static constexpr const char *TOPIC_CONFIG_GET  = "device/waveshare/config/get";
static constexpr const char *TOPIC_CONFIG_SET  = "device/waveshare/config/set";
static constexpr const char *TOPIC_CONFIG_STAT = "device/waveshare/config/status";
static constexpr const char *TOPIC_GEMINI_GET  = "device/waveshare/gemini/get";
static constexpr const char *TOPIC_GEMINI_SET  = "device/waveshare/gemini/set";
static constexpr const char *TOPIC_GEMINI_STAT = "device/waveshare/gemini/status";
static constexpr const char *TOPIC_MEDIA       = "device/waveshare/media";

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
#if CONFIG_WAVESHARE_MQTT_ENABLE
    if (changed & BIT_SYSTEM::WIFI_CONNECTED) {
        bool wifi_ok = snap.system.wifi_connected;

        // 1. React to WiFi Connectivity
        if (wifi_ok && !m_mqtt_handle) {
            LOGI_SYSTEM("WiFi connected — initializing MQTT client.");
            
            esp_mqtt_client_config_t mqtt_cfg = {};

            std::string broker_uri = MQTTS_BROKER_URI;
            std::string username   = MQTT_USERNAME;
            std::string password   = MQTT_PASSWORD;
            std::string client_id  = MQTT_CLIENT_ID;

            // Only override from SD card if an explicit settings file was present with non-default broker
            if (Services::StorageService::getInstance().isMounted() &&
                Services::StorageService::getInstance().fileExists("/sdcard/settings.txt")) {
                Services::SystemConfig sd_cfg = Services::GetConfig();
                if (sd_cfg.mqtt.broker_uri[0] != '\0' && strcmp(sd_cfg.mqtt.broker_uri, "mqtt://broker.emqx.io") != 0) {
                    broker_uri = sd_cfg.mqtt.broker_uri;
                }
                if (sd_cfg.mqtt.username[0] != '\0') username = sd_cfg.mqtt.username;
                if (sd_cfg.mqtt.password[0] != '\0') password = sd_cfg.mqtt.password;
                if (sd_cfg.mqtt.client_id[0] != '\0') client_id = sd_cfg.mqtt.client_id;
            }

            if (broker_uri.find("mqtt://") != 0 && broker_uri.find("mqtts://") != 0 && 
                broker_uri.find("ws://") != 0 && broker_uri.find("wss://") != 0) {
                broker_uri = "mqtts://" + broker_uri;
            }

            ESP_LOGI(TAG, "Connecting to MQTT broker: %s (client_id: %s)", broker_uri.c_str(), client_id.c_str());
            mqtt_cfg.broker.address.uri = broker_uri.c_str();

            // Only set SSL certificate if the URI is a secure scheme (mqtts:// or wss://)
            bool is_tls = (broker_uri.find("mqtts://") == 0 || broker_uri.find("wss://") == 0);
            if (is_tls) {
                mqtt_cfg.broker.verification.certificate = BROKER_ROOT_CA_PEM;
            } else {
                mqtt_cfg.broker.verification.certificate = nullptr;
            }

            if (!username.empty()) {
                mqtt_cfg.credentials.username = username.c_str();
            }
            if (!password.empty()) {
                mqtt_cfg.credentials.authentication.password = password.c_str();
            }
            if (!client_id.empty()) {
                mqtt_cfg.credentials.client_id = client_id.c_str();
            }

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
#endif
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
            esp_mqtt_client_subscribe(m_mqtt_handle, TOPIC_CONFIG, 0);
            esp_mqtt_client_subscribe(m_mqtt_handle, TOPIC_DYNAMIC_SUB, 0);
            esp_mqtt_client_subscribe(m_mqtt_handle, TOPIC_CONFIG_GET, 0);
            esp_mqtt_client_subscribe(m_mqtt_handle, TOPIC_CONFIG_SET, 0);
            esp_mqtt_client_subscribe(m_mqtt_handle, TOPIC_GEMINI_GET, 0);
            esp_mqtt_client_subscribe(m_mqtt_handle, TOPIC_GEMINI_SET, 0);
            esp_mqtt_client_subscribe(m_mqtt_handle, TOPIC_MEDIA, 0);
            
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
    if (event->current_data_offset == 0) {
        m_current_topic.assign(event->topic, event->topic_len);
        m_accumulated_payload.clear();
    }
    m_accumulated_payload.append(event->data, event->data_len);

    if (m_accumulated_payload.length() < event->total_data_len) {
        return; // Wait for more chunks
    }

    // Assemble the complete topic and payload
    std::string topic = m_current_topic;
    std::string payload = m_accumulated_payload;
    m_accumulated_payload.clear();
    m_current_topic.clear();

    if (topic == TOPIC_CONFIG) {
        LOGI_SYSTEM("Processing inbound MQTTS config...");
        Utils::ParserUtils::parseKeyValueStream(payload, onMqttConfigPair, this);
    } else if (topic == TOPIC_CONFIG_GET) {
        std::string config_content = "";
        if (Services::StorageService::getInstance().isMounted() &&
            Services::StorageService::getInstance().fileExists("/sdcard/settings.txt")) {
            config_content = Services::StorageService::getInstance().readFile("/sdcard/settings.txt");
        }
        if (config_content.empty()) {
            config_content = "# No settings.txt present on SD card\n";
        }
        publish(TOPIC_CONFIG_STAT, config_content.c_str());
        ESP_LOGI(TAG, "Published configuration status via MQTT.");
    } else if (topic == TOPIC_CONFIG_SET) {
        if (Services::StorageService::getInstance().isMounted()) {
            if (Services::StorageService::getInstance().writeFile("/sdcard/settings.txt", payload.c_str())) {
                ESP_LOGI(TAG, "Updated settings.txt on SD card via MQTT.");
                Services::LoadConfigFromSD(); // Reload config in config manager
                publish(TOPIC_CONFIG_STAT, "SUCCESS: Settings updated. Restart required to apply changes.");
            } else {
                publish(TOPIC_CONFIG_STAT, "ERROR: Failed to write settings.txt to SD card.");
            }
        } else {
            publish(TOPIC_CONFIG_STAT, "ERROR: SD card not mounted.");
        }
    } else if (topic == TOPIC_GEMINI_GET) {
        std::string gemini_content = "";
        if (Services::StorageService::getInstance().isMounted() &&
            Services::StorageService::getInstance().fileExists("/sdcard/gemini_config.json")) {
            gemini_content = Services::StorageService::getInstance().readFile("/sdcard/gemini_config.json");
        }
        if (gemini_content.empty()) {
            gemini_content = "{}";
        }
        publish(TOPIC_GEMINI_STAT, gemini_content.c_str());
        ESP_LOGI(TAG, "Published gemini config status via MQTT.");
    } else if (topic == TOPIC_GEMINI_SET) {
        if (Services::StorageService::getInstance().isMounted()) {
            if (Services::StorageService::getInstance().writeFile("/sdcard/gemini_config.json", payload.c_str())) {
                ESP_LOGI(TAG, "Updated gemini_config.json on SD card via MQTT.");
                publish(TOPIC_GEMINI_STAT, "SUCCESS: Gemini config updated. Restart required to apply changes.");
            } else {
                publish(TOPIC_GEMINI_STAT, "ERROR: Failed to write gemini_config.json to SD card.");
            }
        } else {
            publish(TOPIC_GEMINI_STAT, "ERROR: SD card not mounted.");
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
    } else if (topic == TOPIC_MEDIA) {
        ESP_LOGI(TAG, "Media command received: %s", payload.c_str());
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);
        if (error) {
            ESP_LOGE(TAG, "Failed to parse media command JSON: %s", error.c_str());
        } else {
            const char* cmd = doc["cmd"];
            const char* query = doc["query"] | doc["title"];

            if (cmd) {
                if (strcmp(cmd, "play") == 0) {
                    if (query && query[0] != '\0') {
                        ESP_LOGI(TAG, "MQTT Media: play '%s'", query);
                        MusicPlaybackService::getInstance().play(query);
                    } else {
                        ESP_LOGI(TAG, "MQTT Media: resume");
                        MusicPlaybackService::getInstance().resume();
                    }
                } else if (strcmp(cmd, "pause") == 0) {
                    ESP_LOGI(TAG, "MQTT Media: pause");
                    MusicPlaybackService::getInstance().pause();
                } else if (strcmp(cmd, "resume") == 0) {
                    ESP_LOGI(TAG, "MQTT Media: resume");
                    MusicPlaybackService::getInstance().resume();
                } else if (strcmp(cmd, "stop") == 0) {
                    ESP_LOGI(TAG, "MQTT Media: stop");
                    MusicPlaybackService::getInstance().stop();
                } else if (strcmp(cmd, "next") == 0) {
                    ESP_LOGI(TAG, "MQTT Media: next");
                    MusicPlaybackService::getInstance().next();
                } else if (strcmp(cmd, "previous") == 0 || strcmp(cmd, "prev") == 0) {
                    ESP_LOGI(TAG, "MQTT Media: previous");
                    MusicPlaybackService::getInstance().previous();
                } else if (strcmp(cmd, "volume") == 0) {
                    int level = doc["level"] | -1;
                    if (level >= 0 && level <= 100) {
                        ESP_LOGI(TAG, "MQTT Media: set volume %d", level);
                        sysdb.mutate([level](SystemState& s) {
                            s.audio.speaker_volume = level;
                        });
                    }
                } else if (strcmp(cmd, "autoplay") == 0) {
                    bool enabled = doc["enabled"] | true;
                    ESP_LOGI(TAG, "MQTT Media: autoplay %s", enabled ? "true" : "false");
                    MusicPlaybackService::getInstance().setAutoplay(enabled);
                } else if (strcmp(cmd, "caching") == 0 || strcmp(cmd, "cache") == 0) {
                    bool enabled = doc["enabled"] | true;
                    ESP_LOGI(TAG, "MQTT Media: caching %s", enabled ? "true" : "false");
                    MusicPlaybackService::getInstance().setCaching(enabled);
                }
            } else if (query && query[0] != '\0') {
                ESP_LOGI(TAG, "MQTT Media: play '%s'", query);
                MusicPlaybackService::getInstance().play(query);
            } else {
                const char* song_id = doc["song_id"];
                const char* song_url = doc["song_url"];
                if (song_id && song_url) {
                    ESP_LOGI(TAG, "Forwarding media command to NexusPlayer: song_id=%s, url=%s", song_id, song_url);
                    NexusPlayer::getInstance().play(song_id, song_url);
                } else {
                    ESP_LOGE(TAG, "Media command payload missing recognized command or song info");
                }
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
        } else if (key == "autoplay") {
            bool enabled = (val == "1" || val == "true");
            MusicPlaybackService::getInstance().setAutoplay(enabled);
            ESP_LOGI(TAG, "Parsed config: autoplay = %d", enabled);
        } else if (key == "cache_downloads" || key == "caching" || key == "cache") {
            bool enabled = (val == "1" || val == "true");
            MusicPlaybackService::getInstance().setCaching(enabled);
            ESP_LOGI(TAG, "Parsed config: cache_downloads = %d", enabled);
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

void MqttService::onMqttConfigPair(const std::string& key, const std::string& val, void* ctx) {
    auto* self = static_cast<MqttService*>(ctx);
    if (key == "speaker_volume" || key == "mic_volume" ||
        key == "sample_rate" || key == "mic_enabled" ||
        key == "autoplay" || key == "cache_downloads" || key == "caching" || key == "cache") {
        self->handleAudioConfig(key, val);
    } else if (key == "led_color") {
        self->handleLedConfig(val);
    }
}
