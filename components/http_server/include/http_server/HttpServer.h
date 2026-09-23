#pragma once

#include "esp_http_server.h"

namespace Http {

using Handler = esp_err_t (*)(httpd_req_t*);

/**
 * @brief Thin owner of an esp_http_server instance plus route registration.
 *
 * App-agnostic: the caller builds the httpd_config_t and registers its routes.
 * Registration failures (e.g. max_uri_handlers exhausted) are logged instead of
 * being silently dropped.
 */
class Server {
public:
    Server() = default;
    ~Server() { stop(); }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(const httpd_config_t& config);
    void stop();
    bool isRunning() const { return m_handle != nullptr; }
    httpd_handle_t handle() const { return m_handle; }

    bool on(const char* uri, httpd_method_t method, Handler handler);

    // WebSocket endpoint. With CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT the
    // server does NOT call the URI handler for the handshake GET: `on_handshake`
    // is the only connect hook and `on_frame` only sees data frames.
    bool onWebSocket(const char* uri, Handler on_frame, Handler on_handshake);

private:
    bool registerUri(const httpd_uri_t& uri);

    httpd_handle_t m_handle = nullptr;
};

} // namespace Http
