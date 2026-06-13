#pragma once

#include "common/ReactorTask.h"
#include "common/app_types.h"
#include "mqtt_client.h"
#include <string>

class MqttService : public ReactorTask {
public:
    static MqttService& getInstance();

    MqttService();
    ~MqttService() override;

    bool begin();

    bool publish(const char* topic, const char* payload, int qos = 1, int retain = 0);

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    static void mqttEventHandlerBridge(void* handler_args,
                                       esp_event_base_t base, int32_t event_id,
                                       void* event_data);

    void handleMqttEvent(int32_t event_id, esp_mqtt_event_handle_t event);
    void processIncomingData(esp_mqtt_event_handle_t event);
    void handleAudioConfig(const std::string& key, const std::string& val);
    void handleLedConfig(const std::string& val);

    esp_mqtt_client_handle_t m_mqtt_handle = nullptr;
    bool m_connected = false;

    static constexpr const char* TAG = "MqttSvc";
};


