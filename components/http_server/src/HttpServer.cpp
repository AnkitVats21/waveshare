#include "http_server/HttpServer.h"
#include "esp_log.h"

namespace Http {

static const char* TAG = "HttpServer";

bool Server::start(const httpd_config_t& config) {
    if (m_handle) return true;

    httpd_config_t cfg = config;
    esp_err_t err = httpd_start(&m_handle, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        m_handle = nullptr;
        return false;
    }
    return true;
}

void Server::stop() {
    if (m_handle) {
        httpd_stop(m_handle);
        m_handle = nullptr;
    }
}

bool Server::registerUri(const httpd_uri_t& uri) {
    if (!m_handle) return false;
    esp_err_t err = httpd_register_uri_handler(m_handle, &uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register %s (method %d): %s", uri.uri, (int)uri.method, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool Server::on(const char* uri, httpd_method_t method, Handler handler) {
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = method;
    u.handler = handler;
    return registerUri(u);
}

bool Server::onWebSocket(const char* uri, Handler on_frame, Handler on_handshake) {
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = HTTP_GET;
    u.handler = on_frame;
    u.is_websocket = true;
    u.handle_ws_control_frames = false;
    u.ws_post_handshake_cb = on_handshake;
    return registerUri(u);
}

} // namespace Http
