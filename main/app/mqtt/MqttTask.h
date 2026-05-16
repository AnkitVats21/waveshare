#pragma once

#include "common/TaskBase.h"
#include "mqtt_client.h"
#include <string>
#include "common/app_types.h"

#define CONFIG_MQTT_BROKER_URI                                                 \
  "mqtts://your-hivemq-broker.cloud:8883"
#define CONFIG_MQTT_USERNAME "your_username"
#define CONFIG_MQTT_PASSWORD "your_password"
#define CONFIG_MQTT_CLIENT_ID "your_client_id"
#define CONFIG_MQTT_LWT_TOPIC "runtime/status"

enum class AppEventId : int32_t {
  MQTT_CONNECTED,
  MQTT_DISCONNECTED,
  OUTGOING_DATA_SUBMIT,
  INCOMING_DATA_RECEIVED
};

class MqttTask : public TaskBase {
private:
  esp_mqtt_client_handle_t m_mqtt_handle = nullptr;

  // Configuration constants
  static constexpr const char *MQTTS_BROKER_URI = CONFIG_MQTT_BROKER_URI;
  static constexpr const char *MQTT_USERNAME = CONFIG_MQTT_USERNAME;
  static constexpr const char *MQTT_PASSWORD = CONFIG_MQTT_PASSWORD;
  static constexpr const char *MQTT_CLIENT_ID = CONFIG_MQTT_CLIENT_ID;
  static constexpr const char *MQTT_LWT_TOPIC = CONFIG_MQTT_LWT_TOPIC;
  static constexpr const char *TOPIC_CONFIG = "device/waveshare/config";
  static constexpr const char *TOPIC_DYNAMIC_SUB = "device/subscribe/topic";

  // Enforce private constructors for Singleton Pattern
  MqttTask(const TaskBase::Config &config);

  // Disable copying and moves entirely
  MqttTask(const MqttTask &) = delete;
  MqttTask &operator=(const MqttTask &) = delete;
  MqttTask(MqttTask &&) = delete;
  MqttTask &operator=(MqttTask &&) = delete;

public:
  /**
   * @brief Access point for the Singleton instance.
   */
  static MqttTask &getInstance();

  /**
   * @brief Safe startup method separating allocation from execution.
   */
  bool init(const GlobalSystemSettings &settings);

protected:
  void run() override;

private:
  static void mqttEventHandlerBridge(void *handler_args, esp_event_base_t base,
                                     int32_t event_id, void *event_data);
  void handleMqttEvent(int32_t event_id, esp_mqtt_event_handle_t event);
  void processIncomingData(esp_mqtt_event_handle_t event);
  static void staticOutgoingDataHandler(void *handler_arg,
                                        esp_event_base_t base, int32_t id,
                                        void *event_data);

  // Config Handlers
  void handleAudioConfig(const std::string &key, const std::string &val);
  void handleLedConfig(const std::string &val);

  struct {
    int speaker_volume;
    float mic_volume;
    uint32_t sample_rate;
    bool mic_enabled;
    struct { uint8_t r, g, b; } led_color;
  } m_cache;
};
