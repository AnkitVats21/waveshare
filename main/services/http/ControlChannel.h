#pragma once

#include "common/ReactorTask.h"
#include "esp_http_server.h"
#include <atomic>
#include <string>

namespace Services {

/**
 * @brief Live control channel for clients: WebSocket at /api/ws on the HTTP server.
 *
 *   - Exactly one client at a time: a new connection takes over and the old
 *     socket is closed (personal device, no multi-client bookkeeping).
 *   - Device -> client: full JSON state on connect, then again on every relevant
 *     SysDb change (coalesced to MIN_PUSH_INTERVAL_MS) and every TELEMETRY_PERIOD_MS.
 *   - Client -> device: {"cmd": "<name>", ...} command envelope.
 *   - Advertises the device over mDNS as nexus.local (_nexus._tcp, _http._tcp).
 *
 * The same state/command format is intended to be reused by a remote relay link
 * later, so the ESP keeps a single protocol regardless of how a client reaches it.
 */
class ControlChannel : public ReactorTask {
public:
    static ControlChannel& getInstance();

    bool begin();

    // Called by HttpService.
    esp_err_t onWsHandshake(httpd_req_t* req);   // ws_post_handshake_cb
    esp_err_t handleWsRequest(httpd_req_t* req); // data frames
    void onSocketClosed(int sockfd);
    void onServerStopped();

    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    ControlChannel();
    ControlChannel(const ControlChannel&) = delete;
    ControlChannel& operator=(const ControlChannel&) = delete;

    void startMdns();
    void pushState();
    void handleCommand(const char* json, size_t len);

    std::atomic<httpd_handle_t> m_server{nullptr};
    std::atomic<int>            m_client_fd{-1};
    std::atomic<bool>           m_push_now{false};

    static constexpr uint32_t MIN_PUSH_INTERVAL_MS = 200;
    static constexpr uint32_t TELEMETRY_PERIOD_MS  = 2000;
    static constexpr size_t   MAX_COMMAND_LEN      = 2048;

    static constexpr const char* MDNS_HOSTNAME = "nexus";
    static constexpr const char* TAG = "ControlCh";
};

} // namespace Services
