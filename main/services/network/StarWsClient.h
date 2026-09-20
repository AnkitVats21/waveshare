#pragma once

#include "common/ReactorTask.h"
#include "core_sysdb/StarProtocol.h"
#include "core_sysdb/EmbeddedSysDb.h"
#include "esp_websocket_client.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <string>
#include <vector>

namespace Services {

/**
 * @brief Outbound STAR WebSocket Client for Waveshare ESP32-S3.
 *
 * Connects outbound to the central Host / Cloud Daemon (e.g. AWS Lightsail / Pi Zero 2W):
 *   - Auto-reconnects with exponential backoff on Wi-Fi connection
 *   - Responds to Host's REQ_CATCHUP with WAL_BATCH or full snapshot
 *   - Dispatches incoming CMD_SET_FIELD and CMD_EXEC_ACTION through SysDb write-gate
 *   - Streams new WAL mutations over WebSocket in real time as FreeRTOS tasks mutate SysDb
 */
class StarWsClient : public ReactorTask {
public:
    static StarWsClient& getInstance();

    StarWsClient();
    ~StarWsClient() override;

    bool begin();
    void stop();

    bool isConnected() const { return m_connected; }

    // ReactorTask interface: react to Wi-Fi connectivity and SysDb mutations
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    StarWsClient(const StarWsClient&) = delete;
    StarWsClient& operator=(const StarWsClient&) = delete;

    esp_websocket_client_handle_t m_client = nullptr;
    std::string m_server_uri;
    bool m_connected = false;
    bool m_synced = false;
    uint32_t m_last_sent_seq = 0;
    SemaphoreHandle_t m_client_mutex = nullptr;

    // Frames produced on the esp_websocket_client library's own internal task
    // (via handleWsData(), called synchronously from its event dispatch —
    // never our ReactorTask context) must never block that task on a network
    // write. They're queued here and drained/sent only from run(), so
    // esp_websocket_client_send_bin() is called from exactly one task.
    QueueHandle_t m_outbound_queue = nullptr;
    static constexpr size_t OUTBOUND_QUEUE_LEN = 16;

    void connectToServer(const char* server_ip);
    void disconnectFromServer();
    void sendFrame(const std::vector<uint8_t>& frame);
    void enqueueFrame(std::vector<uint8_t> frame);
    void drainOutboundQueue();

    static void websocketEventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);
    void handleWsData(const uint8_t* data, size_t len);

    static constexpr const char* TAG = "StarWsClient";
};

} // namespace Services
