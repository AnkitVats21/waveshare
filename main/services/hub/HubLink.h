#pragma once

#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <atomic>
#include <mutex>
#include <string>

namespace Services {

/**
 * @brief The board's one connection to the hub (docs/plans/hub-relay-design.md).
 *
 * A plain ws:// WebSocket on the LAN. The hub is the URL in the hub_url
 * setting, or the first `_nexus-hub._tcp` service mDNS finds. On connect the
 * board sends a hello with the hub token (NVS); the hub answers with a
 * welcome, and from then on the link is up:
 *   - the dashboard's live state and transcript go to the hub
 *     (ControlChannel's relay), which fans them out to its browsers;
 *   - frames with "cmd" and no "type" are those browsers' commands, handled
 *     like /api/ws commands.
 * WebSocket pings every 5 s; no pong for 10 s drops the link. It reconnects
 * on its own; a hub found by mDNS is looked up again after a minute down.
 *
 * Messages arrive on the client's task and are handled on this link's own
 * task (stack in PSRAM).
 */
class HubLink {
public:
    static HubLink& getInstance();

    void start();
    // hub_url or the token changed: drop the link and connect again.
    void reconfigure();

    // Welcomed by the hub.
    bool isUp() const { return m_up.load(); }
    // Sends a text frame; false if the link is down or the send failed.
    bool sendText(const std::string& text);

    struct Status {
        bool up = false;
        bool connected = false;      // socket open (maybe not welcomed yet)
        bool from_mdns = false;
        std::string url;             // the hub in use, "" if none
        std::string hub_version;
        std::string last_error;
    };
    Status status();

private:
    HubLink() = default;
    HubLink(const HubLink&) = delete;
    HubLink& operator=(const HubLink&) = delete;

    static void taskEntry(void* arg);
    void run();
    bool open(const std::string& url);
    void close();
    std::string discover();
    void sendHello();
    void handleMessage(const std::string& text);
    void setDown();
    void setError(const char* error);

    static void onEvent(void* arg, esp_event_base_t base, int32_t id, void* data);

    TaskHandle_t m_task = nullptr;
    QueueHandle_t m_rx_queue = nullptr;   // std::string* frames for the task

    std::mutex m_client_mutex;            // m_client, against close()
    esp_websocket_client_handle_t m_client = nullptr;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_up{false};

    std::mutex m_status_mutex;
    std::string m_url;
    bool m_from_mdns = false;
    std::string m_hub_version;
    std::string m_last_error;

    // Client task only: the frame being reassembled.
    std::string m_rx;
    bool m_rx_dropping = false;

    int64_t m_down_since_us = 0;          // run() only
    bool m_rejected = false;              // the hub refused the hello; run() only

    static constexpr uint32_t BIT_CONNECTED   = 1u << 0;
    static constexpr uint32_t BIT_DOWN        = 1u << 1;
    static constexpr uint32_t BIT_RX          = 1u << 2;
    static constexpr uint32_t BIT_RECONFIGURE = 1u << 3;

    static constexpr size_t MAX_RX_FRAME = 4096;
    static constexpr int RX_QUEUE_LEN = 8;
    static constexpr uint32_t TASK_STACK = 6 * 1024;        // PSRAM
    static constexpr int CLIENT_TASK_STACK = 3584;          // internal (esp_websocket_client); 1.7 KB used
    // The client's rx and tx buffers, one malloc each: above
    // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4096) they go to PSRAM. At 2048
    // they took 4 KB of internal RAM.
    static constexpr int CLIENT_BUFFER = 4608;
    static constexpr uint32_t RETRY_MS = 30 * 1000;         // no hub found
    static constexpr uint32_t REJECTED_RETRY_MS = 5 * 60 * 1000;
    static constexpr int64_t REDISCOVER_US = 60LL * 1000 * 1000;
    static constexpr int RECONNECT_MS = 5000;
    static constexpr int PROTOCOL_VERSION = 1;
    static constexpr const char* DEFAULT_PATH = "/hub/board";
    static constexpr const char* TAG = "HubLink";
};

} // namespace Services
