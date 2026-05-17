#include "MqttTask.h"
#include "app/audio/AudioService.h"
#include "common/AppLogger.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "hal/Board.h"
#include "mqtt_certificates.hpp"
#include "app/event/EventBus.h"
#include <cstdint>
#include <sstream>

MqttTask::MqttTask(const TaskBase::Config &config) : TaskBase(config) {}

MqttTask &MqttTask::getInstance() {
  static TaskBase::Config default_config = {
      .name = "MqttsSingletonTask",
      .stack_size = 6144, // Increased stack size: TLS handshakes require more
                          // stack space than standard TCP
      .priority = 5,
      .core_id = 1};
  static MqttTask instance(default_config);
  return instance;
}

bool MqttTask::init(const GlobalSystemSettings &settings) {
  // Populate initial cache from boot settings
  m_cache.speaker_volume = 80; // Default or read from board
  m_cache.mic_volume = 70.0f;
  m_cache.sample_rate = settings.sample_rate;
  m_cache.mic_enabled = settings.mic_enabled;
  m_cache.led_color = {0, 80, 0}; // Initial green

  return start();
}

void MqttTask::run() {
  ESP_LOGI(m_config.name, "MqttsTask Secure Singleton Thread running...");

  // Secure ESP-MQTT client initialization
  esp_mqtt_client_config_t mqtt_cfg = {};

  // 1. Broker Network URL
  mqtt_cfg.broker.address.uri = MQTTS_BROKER_URI;

  // 2. TLS / SSL Security verification configuration
  mqtt_cfg.broker.verification.certificate = BROKER_ROOT_CA_PEM;

  // 3. User Authentication Credentials
  mqtt_cfg.credentials.username = MQTT_USERNAME;
  mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;
  mqtt_cfg.credentials.client_id = MQTT_CLIENT_ID;

  m_mqtt_handle = esp_mqtt_client_init(&mqtt_cfg);
  if (m_mqtt_handle == nullptr) {
    ESP_LOGE(m_config.name, "Failed to init secure native driver");
    return;
  }

  esp_mqtt_client_register_event(m_mqtt_handle, MQTT_EVENT_ANY,
                                 mqttEventHandlerBridge, this);
  esp_mqtt_client_start(m_mqtt_handle);

  EventBus::getInstance().subscribe(APP_EVENTS,
                                    AppEventId::OUTGOING_DATA_SUBMIT,
                                    staticOutgoingDataHandler, this);

  while (m_running) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  esp_mqtt_client_stop(m_mqtt_handle);
  esp_mqtt_client_destroy(m_mqtt_handle);
}

void MqttTask::mqttEventHandlerBridge(void *handler_args, esp_event_base_t base,
                                      int32_t event_id, void *event_data) {
  MqttTask *self = static_cast<MqttTask *>(handler_args);
  self->handleMqttEvent(event_id,
                        static_cast<esp_mqtt_event_handle_t>(event_data));
}

void MqttTask::handleMqttEvent(int32_t event_id,
                               esp_mqtt_event_handle_t event) {
  EventBus &bus = EventBus::getInstance();
  switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(m_config.name, "Securely connected to broker via TLS!");
    esp_mqtt_client_subscribe(m_mqtt_handle, "device/esp32s3/commands", 0);
    esp_mqtt_client_subscribe(m_mqtt_handle, TOPIC_CONFIG, 0);
    esp_mqtt_client_subscribe(m_mqtt_handle, TOPIC_DYNAMIC_SUB, 0);
    bus.publish(APP_EVENTS, AppEventId::MQTT_CONNECTED, true);
    break;

  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGW(m_config.name, "MQTTS Session Disconnected.");
    bus.publish(APP_EVENTS, AppEventId::MQTT_DISCONNECTED, false);
    break;

  case MQTT_EVENT_DATA:
    ESP_LOGI(m_config.name, "MQTT_EVENT_DATA: Topic=%.*s", event->topic_len,
             event->topic);
    processIncomingData(event);
    break;

  case MQTT_EVENT_ERROR:
    ESP_LOGE(m_config.name, "MQTTS Error Event Detected");
    if (event->error_handle->error_type == MQTT_ERROR_TYPE_ESP_TLS) {
      ESP_LOGE(m_config.name, "Last TLS error code: 0x%x",
               event->error_handle->esp_tls_last_esp_err);
      ESP_LOGE(m_config.name, "Last TLS flags: 0x%x",
               event->error_handle->esp_tls_stack_err);
    }
    break;

  default:
    break;
  }
}

void MqttTask::processIncomingData(esp_mqtt_event_handle_t event) {
  std::string topic(event->topic, event->topic_len);
  std::string payload(event->data, event->data_len);

  if (topic == TOPIC_CONFIG) {
    LOGI_SYSTEM("Processing config update...");
    std::stringstream ss(payload);
    std::string line;
    while (std::getline(ss, line)) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();

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
    new_topic.erase(0, new_topic.find_first_not_of(" \n\r\t"));
    new_topic.erase(new_topic.find_last_not_of(" \n\r\t") + 1);

    if (!new_topic.empty()) {
      int msg_id =
          esp_mqtt_client_subscribe(m_mqtt_handle, new_topic.c_str(), 0);
      if (msg_id >= 0) {
        ESP_LOGI(m_config.name, "Dynamically subscribed to: %s (msg_id=%d)",
                 new_topic.c_str(), msg_id);
      } else {
        ESP_LOGE(m_config.name, "Failed to subscribe to: %s",
                 new_topic.c_str());
      }
    }
  }
}

void MqttTask::handleAudioConfig(const std::string &key,
                                 const std::string &val) {
  try {
    if (key == "speaker_volume") {
      int vol = std::stoi(val);
      if (vol >= 0 && vol <= 100 && vol != m_cache.speaker_volume) {
        m_cache.speaker_volume = vol;
        Board::getInstance().setPlayVolume(vol);
        ESP_LOGI(m_config.name, "Speaker volume updated to %d", vol);
      }
    } else if (key == "mic_volume") {
      float gain = std::stof(val);
      if (gain >= 0 && gain <= 100 && gain != m_cache.mic_volume) {
        m_cache.mic_volume = gain;
        // Send the mic gain to AudioService using the EventBus
        EventBus::getInstance().publish(APP_EVENTS, AppEvent::MIC_GAIN_UPDATE, gain);
        ESP_LOGI(m_config.name, "Published MIC_GAIN_UPDATE event: %.1f", gain);
      }
    } else if (key == "sample_rate") {
      uint32_t sample_rate = std::stoi(val);
      if (sample_rate != m_cache.sample_rate) {
        m_cache.sample_rate = sample_rate;
        AudioService::getInstance().reinit(sample_rate);
        ESP_LOGI(m_config.name, "Sample rate updated to %d", sample_rate);
      }
    } else if (key == "mic_enabled") {
      bool enabled = (val == "1" || val == "true");
      if (enabled != m_cache.mic_enabled) {
        m_cache.mic_enabled = enabled;
        AudioService::getInstance().setMicEnabled(enabled);
        ESP_LOGI(m_config.name, "Mic enabled set to %d", enabled);
      }
    }
  } catch (...) {
    ESP_LOGE(m_config.name, "Failed to parse audio config: %s=%s", key.c_str(),
             val.c_str());
  }
}

void MqttTask::handleLedConfig(const std::string &val) {
  int r, g, b;
  if (sscanf(val.c_str(), "%d,%d,%d", &r, &g, &b) == 3) {
    if (r != m_cache.led_color.r || g != m_cache.led_color.g ||
        b != m_cache.led_color.b) {
      m_cache.led_color = {(uint8_t)r, (uint8_t)g, (uint8_t)b};
      RgbColor color = {(uint8_t)r, (uint8_t)g, (uint8_t)b};
      EventBus::getInstance().publish(APP_EVENTS, AppEvent::LED_COLOR_UPDATE, color);
      ESP_LOGI(m_config.name, "Published LED_COLOR_UPDATE event: %d,%d,%d", r, g, b);
    }
  } else {
    ESP_LOGW(m_config.name, "Invalid led_color format: %s (expected r,g,b)",
             val.c_str());
  }
}

void MqttTask::staticOutgoingDataHandler(void *handler_arg,
                                         esp_event_base_t base, int32_t id,
                                         void *event_data) {
  MqttTask *self = static_cast<MqttTask *>(handler_arg);
  if (self->m_mqtt_handle != nullptr) {
    float *outbound_payload = static_cast<float *>(event_data);
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.2f", *outbound_payload);
    esp_mqtt_client_publish(self->m_mqtt_handle, "device/esp32s3/telemetry",
                            buffer, 0, 1, 0);
  }
}
