#pragma once

#include "common/ReactorTask.h"
#include "core_sysdb/StarProtocol.h"
#include "core_sysdb/EmbeddedSysDb.h"
#include "esp_http_server.h"
#include "freertos/semphr.h"
#include <array>

namespace Services {

/**
 * @brief Authoritative STAR WebSocket Server for Waveshare ESP32-S3.
 *
 * Implements the STAR replication wire protocol:
 *   - Endpoint: GET /api/star/ws (WebSocket binary protocol)
 *   - Serves sequence catch-up queries (REQ_CATCHUP) and full state snapshots
 *   - Validates inbound writes (CMD_SET_FIELD, CMD_EXEC_ACTION) through EmbeddedSysDb write-gate
 *   - Broadcasts real-time WAL_BATCH frames to connected replicas when state mutates
 */
class StarWsServer : public ReactorTask {
public:
    static StarWsServer& getInstance();

    StarWsServer();
    ~StarWsServer() override;

    /**
     * @brief Register the /api/star/ws WebSocket handler with the HTTP daemon.
     */
    bool registerHandler(httpd_handle_t server);

    /**
     * @brief Unregister handler and disconnect clients.
     */
    void unregisterHandler();

    // ReactorTask overrides
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    StarWsServer(const StarWsServer&) = delete;
    StarWsServer& operator=(const StarWsServer&) = delete;

    // HTTP Server & WebSocket handling
    httpd_handle_t m_server = nullptr;
    static esp_err_t wsHandler(httpd_req_t* req);
    esp_err_t processWsFrame(httpd_req_t* req, const uint8_t* data, size_t len);

    // Client tracking
    struct ConnectedClient {
        int      fd = -1;
        uint32_t last_seq = 0;
        bool     synced = false;
    };
    static constexpr size_t MAX_STAR_CLIENTS = 4;
    std::array<ConnectedClient, MAX_STAR_CLIENTS> m_clients{};
    SemaphoreHandle_t m_clients_mutex = nullptr;

    void trackClient(int fd, uint32_t last_seq, bool synced);
    void updateClientSeq(int fd, uint32_t seq);
    void removeClient(int fd);
    void broadcastWalUpdates();

    static constexpr const char* TAG = "StarWsServer";
};

} // namespace Services
