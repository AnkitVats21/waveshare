#pragma once

#include "common/IService.h"
#include "common/TaskBase.h"
#include "common/events/app_events.h"
#include "common/events/mqtt_events.h"
#include "common/system_settings.h"
#include "mqtt_client.h"
#include <string>

/**
 * @brief Secure MQTT service (TLS/MQTTS).
 *
 * Extends IService for standardized event subscription and dispatch.
 * Extends TaskBase for its own FreeRTOS task.
 *
 * MQTT credentials are configured via Kconfig (idf.py menuconfig →
 * "Waveshare Audio Development Board Config").
 */
class MqttTask : public IService, public TaskBase {
public:
  static MqttTask &getInstance();

  /**
   * @brief Start the MQTT task.
   * @note  Kept for backward compat; internally calls onStart().
   */
  bool init(const GlobalSystemSettings &settings);

  // IService interface
  bool onStart() override;

protected:
  void run() override;

private:
  explicit MqttTask(const TaskBase::Config &config);
  MqttTask(const MqttTask &) = delete;
  MqttTask &operator=(const MqttTask &) = delete;

  static void mqttEventHandlerBridge(void *handler_args,
                                     esp_event_base_t base, int32_t event_id,
                                     void *event_data);

  static void staticOutgoingDataHandler(void *handler_arg,
                                        esp_event_base_t base, int32_t id,
                                        void *event_data);

  void handleMqttEvent(int32_t event_id, esp_mqtt_event_handle_t event);
  void processIncomingData(esp_mqtt_event_handle_t event);
  void handleAudioConfig(const std::string &key, const std::string &val);
  void handleLedConfig(const std::string &val);

  esp_mqtt_client_handle_t m_mqtt_handle = nullptr;

  struct Cache {
    int      speaker_volume;
    float    mic_volume;
    uint32_t sample_rate;
    bool     mic_enabled;
    struct { uint8_t r, g, b; } led_color;
  } m_cache{};
};

// ---------------------------------------------------------------------------
// Type alias: MqttService is the preferred name going forward.
// MqttTask remains the class name for now to avoid touching all call-sites.
// ---------------------------------------------------------------------------
using MqttService = MqttTask;
