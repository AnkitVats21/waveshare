#pragma once

#include "common/ReactorTask.h"
#include "http_server/HttpServer.h"

namespace Services {

/**
 * @brief Runs the device HTTP server (REST API, /api/ws, dashboard, captive
 * portal) while the network is up: STA connected or the setup SoftAP active.
 *
 * Endpoints live in services/http/routes; this class only owns the server's
 * lifecycle and configuration.
 */
class HttpService : public ReactorTask {
public:
    static HttpService& getInstance();

    bool begin();

    // ReactorTask interface: react to Wi-Fi connectivity in EmbeddedSysDb
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

    bool isRunning() const { return m_server.isRunning(); }

protected:
    void run() override;

private:
    HttpService();

    bool startServer();
    void stopServer();

    Http::Server m_server;
    bool m_network_up = false;

    static constexpr const char* TAG = "HttpService";
};

} // namespace Services
