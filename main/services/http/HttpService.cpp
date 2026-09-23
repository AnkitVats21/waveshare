#include "services/http/HttpService.h"
#include "services/http/ControlChannel.h"
#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "http_server/WebBundle.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <unistd.h>

#ifndef CONFIG_WAVESHARE_HTTP_FILE_SERVER_PORT
#define CONFIG_WAVESHARE_HTTP_FILE_SERVER_PORT 80
#endif

namespace Services {

HttpService& HttpService::getInstance() {
    static HttpService instance;
    return instance;
}

HttpService::HttpService()
    : ReactorTask({
          "HttpSvc",
          ThreadConfig::StackSize::STACK_NORMAL,
          ThreadConfig::Priority::LOW,
          ThreadConfig::CORE_NETWORK,
          COMP::SYSTEM
      }) {
}

bool HttpService::begin() {
    // Maps the active frontend slot; must run on a task with an internal stack
    // (app_main), not on the httpd task.
    Http::WebBundle::instance().init();
    onStateChanged(COMP::SYSTEM, EmbeddedSysDb::getInstance().snapshot());
    if (!m_network_up) {
        ESP_LOGI(TAG, "Initialized, awaiting Wi-Fi connection...");
    }
    return true;
}

void HttpService::onStateChanged(ComponentMask changed, const SystemState& snap) {
    if (!(changed & COMP::SYSTEM)) return;

    bool should_run = snap.system.wifi_connected || snap.system.ap_active;
    if (should_run && !m_network_up) {
        m_network_up = true;
        ESP_LOGI(TAG, "Network active (%s) — starting HTTP server on port %d...",
                 snap.system.ap_active ? "SoftAP" : "STA", CONFIG_WAVESHARE_HTTP_FILE_SERVER_PORT);
        startServer();
    } else if (!should_run && m_network_up) {
        m_network_up = false;
        ESP_LOGI(TAG, "Network down — stopping HTTP server...");
        stopServer();
    }
}

void HttpService::run() {
    while (m_running) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!m_running) break;
        onStateChanged(COMP::SYSTEM, EmbeddedSysDb::getInstance().snapshot());
    }
}

bool HttpService::startServer() {
    if (m_server.isRunning()) return true;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_WAVESHARE_HTTP_FILE_SERVER_PORT;
    config.ctrl_port = config.server_port + 32000;
    config.task_priority = ThreadConfig::Priority::LOW;
    config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT; // keep internal SRAM for audio/Wi-Fi
    config.stack_size = 12288;
    config.core_id = ThreadConfig::CORE_NETWORK;
    config.max_uri_handlers = 64;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_open_sockets = 12;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.lru_purge_enable = true;
    config.close_fn = [](httpd_handle_t, int sockfd) {
        ControlChannel::getInstance().onSocketClosed(sockfd);
        close(sockfd);
    };

    if (!m_server.start(config)) return false;

    Routes::registerWeb(m_server);
    m_server.onWebSocket("/api/ws",
        [](httpd_req_t* req) { return ControlChannel::getInstance().handleWsRequest(req); },
        [](httpd_req_t* req) { return ControlChannel::getInstance().onWsHandshake(req); });
    Routes::registerWifi(m_server);
    Routes::registerFiles(m_server);
    Routes::registerAudio(m_server);
    Routes::registerMusic(m_server);
    Routes::registerSystem(m_server);
    Routes::registerConfig(m_server);
    Routes::registerOta(m_server);
    // CORS preflight for every /api/* endpoint.
    m_server.on("/api/*", HTTP_OPTIONS, Http::corsPreflight);
    // Wildcard GET must come after every other GET route (first match wins).
    Routes::registerFrontend(m_server);

    ESP_LOGI(TAG, "Listening on port %d", config.server_port);
    return true;
}

void HttpService::stopServer() {
    if (!m_server.isRunning()) return;
    ESP_LOGI(TAG, "Stopping HTTP server...");
    m_server.stop();
    ControlChannel::getInstance().onServerStopped();
}

} // namespace Services
