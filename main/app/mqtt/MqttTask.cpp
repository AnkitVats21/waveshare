#include "MqttTask.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "mqtt_certificates.hpp"
#include "services/EventBus.h"

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

bool MqttTask::init() { return start(); }

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

  esp_mqtt_client_register_event(m_mqtt_handle,
                                 (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
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
    bus.publish(APP_EVENTS, AppEventId::MQTT_CONNECTED, true);
    break;

  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGW(m_config.name, "MQTTS Session Disconnected.");
    bus.publish(APP_EVENTS, AppEventId::MQTT_DISCONNECTED, false);
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
