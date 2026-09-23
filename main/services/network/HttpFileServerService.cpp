#include "HttpFileServerService.h"
#include "ControlChannel.h"
#include "WebDashboardHtml.h"
#include "CaptivePortalHtml.h"
#include "services/storage/StorageService.h"
#include "common/AppLogger.h"
#include "common/thread_config.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "core_sysdb/LogRouter.h"
#include "core_sysdb/led_types.h"
#include "app/audio/recording/AudioRecorder.h"
#include "audio_core/AlertPlayer.h"
#include "media_player/MusicPlaybackService.h"
#include "media_player/CatalogDB.h"
#include "media_player/NexusPlayer.h"

#include <ArduinoJson.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_app_desc.h>
#include <freertos/idf_additions.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>

#ifndef CONFIG_WAVESHARE_HTTP_FILE_SERVER_PORT
#define CONFIG_WAVESHARE_HTTP_FILE_SERVER_PORT 80
#endif

#ifndef CONFIG_WAVESHARE_HTTP_FILE_SERVER_BASE_PATH
#define CONFIG_WAVESHARE_HTTP_FILE_SERVER_BASE_PATH "/sdcard"
#endif

namespace Services {

HttpFileServerService& HttpFileServerService::getInstance() {
    static HttpFileServerService instance;
    return instance;
}

HttpFileServerService::HttpFileServerService()
    : ReactorTask({
          "HttpFileSvc",
          ThreadConfig::StackSize::STACK_NORMAL,
          ThreadConfig::Priority::LOW,
          ThreadConfig::CORE_NETWORK,
          COMP::SYSTEM
      }) {
}

HttpFileServerService::~HttpFileServerService() {
    stopServer();
}

bool HttpFileServerService::begin() {
    SystemState snap = EmbeddedSysDb::getInstance().snapshot();
    if (snap.system.wifi_connected || snap.system.ap_active) {
        m_wifi_was_connected = true;
        ESP_LOGI(TAG, "Network active (%s) at boot — starting Control Hub & HTTP server on port %d...",
                 snap.system.ap_active ? "SoftAP" : "STA",
                 CONFIG_WAVESHARE_HTTP_FILE_SERVER_PORT);
        startServer();
    } else {
        ESP_LOGI(TAG, "HttpFileServerService initialized, awaiting Wi-Fi connection...");
    }
    return true;
}

void HttpFileServerService::onStateChanged(ComponentMask changed, const SystemState& snap) {
    if (!(changed & COMP::SYSTEM)) {
        return;
    }

    bool should_run = snap.system.wifi_connected || snap.system.ap_active;
    if (should_run && !m_wifi_was_connected) {
        m_wifi_was_connected = true;
        ESP_LOGI(TAG, "Network active (%s) — starting Control Hub & HTTP server on port %d...",
                 snap.system.ap_active ? "SoftAP" : "STA",
                 CONFIG_WAVESHARE_HTTP_FILE_SERVER_PORT);
        startServer();
    } else if (!should_run && m_wifi_was_connected) {
        m_wifi_was_connected = false;
        ESP_LOGI(TAG, "Network down — stopping Control Hub server...");
        stopServer();
    }
}

void HttpFileServerService::run() {
    while (m_running) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!m_running) break;

        SystemState snap = EmbeddedSysDb::getInstance().snapshot();
        onStateChanged(COMP::SYSTEM, snap);
    }
}

bool HttpFileServerService::startServer() {
    if (m_server != nullptr) {
        ESP_LOGW(TAG, "Server is already running.");
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_WAVESHARE_HTTP_FILE_SERVER_PORT;
    config.ctrl_port = config.server_port + 32000;
    config.task_priority = ThreadConfig::Priority::LOW;
    config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT; // Keeps 100% of internal SRAM free for audio/Wi-Fi!
    config.stack_size = 12288;
    config.core_id = ThreadConfig::CORE_NETWORK;
    config.max_uri_handlers = 48;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_open_sockets = 12;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.lru_purge_enable = true;
    config.close_fn = [](httpd_handle_t, int sockfd) {
        ControlChannel::getInstance().onSocketClosed(sockfd);
        close(sockfd);
    };

    esp_err_t ret = httpd_start(&m_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        m_server = nullptr;
        return false;
    }

    registerUriHandlers();
    ESP_LOGI(TAG, "Waveshare Control Hub & Web Dashboard listening on port %d", config.server_port);
    return true;
}

void HttpFileServerService::stopServer() {
    if (m_server != nullptr) {
        ESP_LOGI(TAG, "Stopping HTTP Server...");
        httpd_stop(m_server);
        ControlChannel::getInstance().onServerStopped();
        m_server = nullptr;
    }
}

void HttpFileServerService::registerUriHandlers() {
    if (!m_server) return;

    auto reg = [this](const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
        httpd_uri_t u = {
            .uri      = uri,
            .method   = method,
            .handler  = handler,
            .user_ctx = nullptr,
            .is_websocket = false,
            .handle_ws_control_frames = false,
            .supported_subprotocol = nullptr,
            .ws_post_handshake_cb = nullptr
        };
        httpd_register_uri_handler(m_server, &u);
    };

    // 1. Dashboard Web UI & Captive Portal
    reg("/", HTTP_GET, indexHandler);
    reg("/index.html", HTTP_GET, indexHandler);
    reg("/setup", HTTP_GET, indexHandler);

    // Captive Portal Detection Redirects
    reg("/generate_204", HTTP_GET, captiveRedirectHandler);
    reg("/gen_204", HTTP_GET, captiveRedirectHandler);
    reg("/hotspot-detect.html", HTTP_GET, captiveRedirectHandler);
    reg("/connecttest.txt", HTTP_GET, captiveRedirectHandler);
    reg("/ncsi.txt", HTTP_GET, captiveRedirectHandler);
    reg("/canonical.html", HTTP_GET, captiveRedirectHandler);

    // Live control channel (single client, takeover) — see ControlChannel
    {
        httpd_uri_t ws = {
            .uri      = "/api/ws",
            .method   = HTTP_GET,
            .handler  = [](httpd_req_t* req) { return ControlChannel::getInstance().handleWsRequest(req); },
            .user_ctx = nullptr,
            .is_websocket = true,
            .handle_ws_control_frames = false,
            .supported_subprotocol = nullptr,
            // With CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT the server does NOT call
            // .handler for the handshake GET; this callback is the only connect hook.
            .ws_post_handshake_cb = [](httpd_req_t* req) { return ControlChannel::getInstance().onWsHandshake(req); }
        };
        httpd_register_uri_handler(m_server, &ws);
    }

    // Wi-Fi Setup & Provisioning APIs
    reg("/api/wifi/scan", HTTP_GET, wifiScanHandler);
    reg("/api/wifi/configure", HTTP_POST, wifiConfigureHandler);
    reg("/api/wifi/status", HTTP_GET, wifiStatusHandler);

    // 2. File Manager APIs
    reg("/api/storage/info", HTTP_GET, storageInfoHandler);
    reg("/api/files", HTTP_GET, listFilesHandler);
    reg("/api/files/download", HTTP_GET, downloadFileHandler);
    reg("/api/files/upload", HTTP_POST, uploadFileHandler);
    reg("/api/files/mkdir", HTTP_POST, mkdirHandler);
    reg("/api/files/rename", HTTP_POST, renameHandler);
    reg("/api/files", HTTP_DELETE, deleteHandler);

    // 3. Audio & Voice Controls
    reg("/api/audio/volume", HTTP_POST, audioVolumeHandler);
    reg("/api/audio/mic_gain", HTTP_POST, audioMicGainHandler);
    reg("/api/audio/mic_mute", HTTP_POST, audioMicMuteHandler);
    reg("/api/audio/alert", HTTP_POST, audioAlertHandler);
    reg("/api/audio/record/start", HTTP_POST, audioRecordHandler);
    reg("/api/audio/record/stop", HTTP_POST, audioRecordHandler);

    // 4. Music & Library APIs
    reg("/api/music/play", HTTP_POST, musicPlayHandler);
    reg("/api/music/play_local", HTTP_POST, musicPlayLocalHandler);
    reg("/api/music/control", HTTP_POST, musicControlHandler);
    reg("/api/music/status", HTTP_GET, musicStatusHandler);
    reg("/api/music/library", HTTP_GET, musicLibraryHandler);
    reg("/api/music/library/scan", HTTP_POST, musicLibraryScanHandler);
    reg("/api/music/library", HTTP_DELETE, musicLibraryDeleteHandler);

    // 5. LED Lighting Controls
    reg("/api/led/set", HTTP_POST, ledSetHandler);

    // 5. System Metrics & Telemetry
    reg("/api/system/metrics", HTTP_GET, metricsHandler);
    reg("/api/system/init", HTTP_GET, systemInitHandler);
    reg("/api/system/delta", HTTP_GET, systemDeltaHandler);

    // 6. Config Management
    reg("/api/config/settings", HTTP_GET, configGetHandler);
    reg("/api/config/settings", HTTP_POST, configSetHandler);
    reg("/api/config/gemini", HTTP_GET, configGetHandler);
    reg("/api/config/gemini", HTTP_POST, configSetHandler);

    // 7. Live Console Logs
    reg("/api/logs", HTTP_GET, logsHandler);

    // 8. OTA Firmware Update & System
    reg("/api/ota/status", HTTP_GET, otaStatusHandler);
    reg("/api/ota", HTTP_POST, otaUploadHandler);
    reg("/api/system/reboot", HTTP_POST, systemRebootHandler);

    // 9. CORS Preflight Handler (wildcard covers all /api/* endpoints including /api/files)
    reg("/api/*", HTTP_OPTIONS, optionsHandler);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

void HttpFileServerService::urlDecode(const std::string& in, std::string& out) {
    out.clear();
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); ++i) {
        if (in[i] == '%') {
            if (i + 2 < in.length()) {
                char hex[3] = { in[i+1], in[i+2], '\0' };
                char* end = nullptr;
                long val = strtol(hex, &end, 16);
                if (end != hex) {
                    out += static_cast<char>(val);
                    i += 2;
                } else {
                    out += '%';
                }
            } else {
                out += '%';
            }
        } else if (in[i] == '+') {
            out += ' ';
        } else {
            out += in[i];
        }
    }
}

bool HttpFileServerService::getQueryParam(httpd_req_t* req, const char* param_name, std::string& out_val) {
    out_val.clear();
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0) return false;

    std::string query_str(query_len + 1, '\0');
    if (httpd_req_get_url_query_str(req, &query_str[0], query_len + 1) != ESP_OK) return false;

    std::string param_buf(query_len + 1, '\0');
    if (httpd_query_key_value(query_str.c_str(), param_name, &param_buf[0], query_len + 1) != ESP_OK) return false;

    std::string raw_val = param_buf.c_str();
    urlDecode(raw_val, out_val);
    return true;
}

esp_err_t HttpFileServerService::sendJsonResponse(httpd_req_t* req, int status_code, const std::string& json_str) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_type(req, "application/json");

    if (status_code == 200) httpd_resp_set_status(req, "200 OK");
    else if (status_code == 400) httpd_resp_set_status(req, "400 Bad Request");
    else if (status_code == 404) httpd_resp_set_status(req, "404 Not Found");
    else if (status_code == 500) httpd_resp_set_status(req, "500 Internal Server Error");
    else {
        char status_buf[32];
        snprintf(status_buf, sizeof(status_buf), "%d Status", status_code);
        httpd_resp_set_status(req, status_buf);
    }

    return httpd_resp_send(req, json_str.c_str(), json_str.length());
}

esp_err_t HttpFileServerService::sendJsonError(httpd_req_t* req, int status_code, const char* message) {
    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = message;
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, status_code, out);
}

const char* HttpFileServerService::getMimeType(const std::string& path) {
    const char* ext = strrchr(path.c_str(), '.');
    if (!ext) return "application/octet-stream";
    if (strcasecmp(ext, ".wav") == 0) return "audio/wav";
    if (strcasecmp(ext, ".mp3") == 0) return "audio/mpeg";
    if (strcasecmp(ext, ".json") == 0) return "application/json";
    if (strcasecmp(ext, ".txt") == 0) return "text/plain";
    if (strcasecmp(ext, ".html") == 0) return "text/html";
    if (strcasecmp(ext, ".csv") == 0) return "text/csv";
    return "application/octet-stream";
}

// ─────────────────────────────────────────────────────────────────────────────
// Files Handlers
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t HttpFileServerService::indexHandler(httpd_req_t* req) {
    SystemState snap = EmbeddedSysDb::getInstance().snapshot();
    httpd_resp_set_type(req, "text/html");
    if (snap.system.ap_active || std::strstr(req->uri, "/setup") != nullptr) {
        return httpd_resp_send(req, CAPTIVE_PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send(req, WEB_DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t HttpFileServerService::captiveRedirectHandler(httpd_req_t* req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t HttpFileServerService::wifiScanHandler(httpd_req_t* req) {
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = true;

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return sendJsonError(req, 500, "Wi-Fi scan failed");
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num > 25) ap_num = 25;

    std::vector<wifi_ap_record_t> ap_records(ap_num);
    if (ap_num > 0) {
        esp_wifi_scan_get_ap_records(&ap_num, ap_records.data());
    }

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (uint16_t i = 0; i < ap_num; ++i) {
        if (ap_records[i].ssid[0] == '\0') continue;
        JsonObject ap = arr.add<JsonObject>();
        ap["ssid"] = reinterpret_cast<char*>(ap_records[i].ssid);
        ap["rssi"] = ap_records[i].rssi;
        ap["auth"] = (ap_records[i].authmode == WIFI_AUTH_OPEN) ? "OPEN" : "SECURED";
    }

    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::wifiConfigureHandler(httpd_req_t* req) {
    char buf[256];
    int total_len = req->content_len;
    if (total_len >= sizeof(buf) || total_len <= 0) {
        return sendJsonError(req, 400, "Invalid payload length");
    }

    int cur_len = 0;
    while (cur_len < total_len) {
        int received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) return sendJsonError(req, 500, "Failed to read request body");
        cur_len += received;
    }
    buf[total_len] = '\0';

    JsonDocument doc;
    DeserializationError derr = deserializeJson(doc, buf);
    if (derr || !doc["ssid"].is<const char*>()) {
        return sendJsonError(req, 400, "JSON must contain 'ssid'");
    }

    std::string ssid = doc["ssid"].as<std::string>();
    std::string pass = doc["password"] | "";

    if (ssid.empty()) {
        return sendJsonError(req, 400, "SSID cannot be empty");
    }

    ESP_LOGI(TAG, "Wi-Fi credentials received for SSID '%s', initiating connection...", ssid.c_str());

    EmbeddedSysDb::getInstance().mutate([&ssid, &pass](SystemState& s) {
        std::strncpy(s.system.wifi_ssid, ssid.c_str(), sizeof(s.system.wifi_ssid) - 1);
        s.system.wifi_ssid[sizeof(s.system.wifi_ssid) - 1] = '\0';
        std::strncpy(s.system.wifi_password, pass.c_str(), sizeof(s.system.wifi_password) - 1);
        s.system.wifi_password[sizeof(s.system.wifi_password) - 1] = '\0';
        s.system.network_state = NetworkState::Connecting;
        s.system.wifi_apply_creds = true;
    });

    JsonDocument resp;
    resp["status"] = "ok";
    resp["message"] = "Credentials received. Connecting to Wi-Fi...";
    std::string out;
    serializeJson(resp, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::wifiStatusHandler(httpd_req_t* req) {
    SystemState snap = EmbeddedSysDb::getInstance().snapshot();

    JsonDocument doc;
    const char* state_str = "Disconnected";
    switch (snap.system.network_state) {
        case NetworkState::Connecting:   state_str = "Connecting"; break;
        case NetworkState::Connected:    state_str = "Connected"; break;
        case NetworkState::PortalActive: state_str = "PortalActive"; break;
        case NetworkState::Failed:       state_str = "Failed"; break;
        default: break;
    }

    doc["network_state"] = state_str;
    doc["wifi_connected"] = snap.system.wifi_connected;
    doc["ap_active"] = snap.system.ap_active;
    doc["ssid"] = snap.system.wifi_ssid;

    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::optionsHandler(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t HttpFileServerService::storageInfoHandler(httpd_req_t* req) {
    uint64_t total_bytes = 0, free_bytes = 0;
    bool mounted = StorageService::getInstance().isMounted();
    if (mounted) {
        StorageService::getInstance().getStorageInfo(
            CONFIG_WAVESHARE_HTTP_FILE_SERVER_BASE_PATH, total_bytes, free_bytes);
    }
    JsonDocument doc;
    doc["mounted"] = mounted;
    doc["total_bytes"] = total_bytes;
    doc["free_bytes"] = free_bytes;
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::listFilesHandler(httpd_req_t* req) {
    if (!StorageService::getInstance().isMounted()) return sendJsonError(req, 500, "SD Card not mounted");

    std::string raw_path;
    if (!getQueryParam(req, "path", raw_path) || raw_path.empty()) {
        raw_path = CONFIG_WAVESHARE_HTTP_FILE_SERVER_BASE_PATH;
    }

    std::string sanitized_path;
    if (!StorageService::sanitizePath(raw_path.c_str(), sanitized_path, CONFIG_WAVESHARE_HTTP_FILE_SERVER_BASE_PATH)) {
        return sendJsonError(req, 400, "Invalid path traversal");
    }

    std::vector<StorageService::FileEntryInfo> entries =
        StorageService::getInstance().listDirectoryDetailed(sanitized_path.c_str());

    JsonDocument doc;
    doc["path"] = sanitized_path;
    JsonArray arr = doc["entries"].to<JsonArray>();

    for (const auto& item : entries) {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = item.name;
        obj["size"] = item.size;
        obj["is_dir"] = item.is_dir;
        obj["mtime"] = static_cast<uint64_t>(item.mtime);
    }

    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::downloadFileHandler(httpd_req_t* req) {
    if (!StorageService::getInstance().isMounted()) return sendJsonError(req, 500, "SD Card not mounted");

    std::string raw_path;
    if (!getQueryParam(req, "path", raw_path) || raw_path.empty()) return sendJsonError(req, 400, "Missing path");

    std::string sanitized_path;
    if (!StorageService::sanitizePath(raw_path.c_str(), sanitized_path, CONFIG_WAVESHARE_HTTP_FILE_SERVER_BASE_PATH)) {
        return sendJsonError(req, 400, "Invalid path traversal");
    }

    FILE* f = fopen(sanitized_path.c_str(), "rb");
    if (!f) return sendJsonError(req, 404, "File not found");

    httpd_resp_set_type(req, getMimeType(sanitized_path));
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    const char* filename = strrchr(sanitized_path.c_str(), '/');
    filename = (filename != nullptr) ? filename + 1 : sanitized_path.c_str();

    char disp_hdr[128];
    snprintf(disp_hdr, sizeof(disp_hdr), "inline; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp_hdr);

    constexpr size_t CHUNK_SIZE = 4096;
    std::vector<char> chunk_buf(CHUNK_SIZE);

    while (true) {
        size_t read_bytes = 0;
        {
            StorageService::getInstance().lock();
            read_bytes = fread(chunk_buf.data(), 1, CHUNK_SIZE, f);
            StorageService::getInstance().unlock();
        }

        if (read_bytes == 0) break;

        esp_err_t send_res = httpd_resp_send_chunk(req, chunk_buf.data(), read_bytes);
        if (send_res != ESP_OK) {
            fclose(f);
            return send_res;
        }
    }

    fclose(f);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t HttpFileServerService::uploadFileHandler(httpd_req_t* req) {
    if (!StorageService::getInstance().isMounted()) return sendJsonError(req, 500, "SD Card not mounted");

    std::string raw_path;
    if (!getQueryParam(req, "path", raw_path) || raw_path.empty()) return sendJsonError(req, 400, "Missing path");

    std::string sanitized_path;
    if (!StorageService::sanitizePath(raw_path.c_str(), sanitized_path, CONFIG_WAVESHARE_HTTP_FILE_SERVER_BASE_PATH)) {
        return sendJsonError(req, 400, "Invalid path traversal");
    }

    size_t last_slash = sanitized_path.rfind('/');
    if (last_slash != std::string::npos && last_slash > 0) {
        std::string parent_dir = sanitized_path.substr(0, last_slash);
        StorageService::getInstance().createDirectory(parent_dir.c_str());
    }

    FILE* f = fopen(sanitized_path.c_str(), "wb");
    if (!f) return sendJsonError(req, 500, "Cannot open target file for writing");

    constexpr size_t CHUNK_SIZE = 4096;
    std::vector<char> chunk_buf(CHUNK_SIZE);
    int remaining = req->content_len;

    while (remaining > 0) {
        int to_read = (remaining < static_cast<int>(CHUNK_SIZE)) ? remaining : static_cast<int>(CHUNK_SIZE);
        int received = httpd_req_recv(req, chunk_buf.data(), to_read);

        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            fclose(f);
            remove(sanitized_path.c_str());
            return sendJsonError(req, 500, "Upload socket transfer failed");
        }

        size_t written = 0;
        {
            StorageService::getInstance().lock();
            written = fwrite(chunk_buf.data(), 1, received, f);
            StorageService::getInstance().unlock();
        }

        if (written != static_cast<size_t>(received)) {
            fclose(f);
            remove(sanitized_path.c_str());
            return sendJsonError(req, 507, "Disk full or write failed");
        }

        remaining -= received;
    }

    fclose(f);
    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "File uploaded successfully";
    doc["path"] = sanitized_path;
    doc["bytes"] = req->content_len;
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::mkdirHandler(httpd_req_t* req) {
    if (!StorageService::getInstance().isMounted()) return sendJsonError(req, 500, "SD Card not mounted");

    std::string raw_path;
    if (!getQueryParam(req, "path", raw_path) || raw_path.empty()) {
        if (req->content_len > 0 && req->content_len < 512) {
            std::string body(req->content_len + 1, '\0');
            int ret = httpd_req_recv(req, &body[0], req->content_len);
            if (ret > 0) {
                JsonDocument doc;
                if (!deserializeJson(doc, body.c_str())) {
                    const char* p = doc["path"];
                    if (p) raw_path = p;
                }
            }
        }
    }

    if (raw_path.empty()) return sendJsonError(req, 400, "Missing path");

    std::string sanitized_path;
    if (!StorageService::sanitizePath(raw_path.c_str(), sanitized_path, CONFIG_WAVESHARE_HTTP_FILE_SERVER_BASE_PATH)) {
        return sendJsonError(req, 400, "Invalid path traversal");
    }

    if (!StorageService::getInstance().createDirectory(sanitized_path.c_str())) {
        return sendJsonError(req, 500, "Failed to create directory");
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Directory created";
    doc["path"] = sanitized_path;
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::renameHandler(httpd_req_t* req) {
    if (!StorageService::getInstance().isMounted()) return sendJsonError(req, 500, "SD Card not mounted");

    std::string old_path, new_path;
    if (!getQueryParam(req, "old_path", old_path) || !getQueryParam(req, "new_path", new_path)) {
        if (req->content_len > 0 && req->content_len < 1024) {
            std::string body(req->content_len + 1, '\0');
            int ret = httpd_req_recv(req, &body[0], req->content_len);
            if (ret > 0) {
                JsonDocument doc;
                if (!deserializeJson(doc, body.c_str())) {
                    const char* op = doc["old_path"];
                    const char* np = doc["new_path"];
                    if (op) old_path = op;
                    if (np) new_path = np;
                }
            }
        }
    }

    if (old_path.empty() || new_path.empty()) return sendJsonError(req, 400, "Missing old_path or new_path");

    std::string sanitized_old, sanitized_new;
    if (!StorageService::sanitizePath(old_path.c_str(), sanitized_old, CONFIG_WAVESHARE_HTTP_FILE_SERVER_BASE_PATH) ||
        !StorageService::sanitizePath(new_path.c_str(), sanitized_new, CONFIG_WAVESHARE_HTTP_FILE_SERVER_BASE_PATH)) {
        return sendJsonError(req, 400, "Invalid path traversal");
    }

    if (!StorageService::getInstance().renamePath(sanitized_old.c_str(), sanitized_new.c_str())) {
        return sendJsonError(req, 500, "Failed to rename path");
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Renamed successfully";
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::deleteHandler(httpd_req_t* req) {
    if (!StorageService::getInstance().isMounted()) return sendJsonError(req, 500, "SD Card not mounted");

    std::string raw_path;
    if (!getQueryParam(req, "path", raw_path) || raw_path.empty()) return sendJsonError(req, 400, "Missing path");

    std::string sanitized_path;
    if (!StorageService::sanitizePath(raw_path.c_str(), sanitized_path, CONFIG_WAVESHARE_HTTP_FILE_SERVER_BASE_PATH)) {
        return sendJsonError(req, 400, "Invalid path traversal");
    }

    if (sanitized_path == CONFIG_WAVESHARE_HTTP_FILE_SERVER_BASE_PATH ||
        sanitized_path == std::string(CONFIG_WAVESHARE_HTTP_FILE_SERVER_BASE_PATH) + "/") {
        return sendJsonError(req, 403, "Cannot delete root SD mount point");
    }

    if (!StorageService::getInstance().deletePath(sanitized_path.c_str())) {
        return sendJsonError(req, 500, "Failed to delete target");
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Deleted successfully";
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

// ─────────────────────────────────────────────────────────────────────────────
// Audio & Voice Handlers
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t HttpFileServerService::audioVolumeHandler(httpd_req_t* req) {
    std::string val_str;
    int vol = -1;
    if (getQueryParam(req, "value", val_str)) {
        vol = atoi(val_str.c_str());
    } else if (req->content_len > 0) {
        std::string body(req->content_len + 1, '\0');
        httpd_req_recv(req, &body[0], req->content_len);
        JsonDocument doc;
        if (!deserializeJson(doc, body.c_str())) {
            vol = doc["volume"] | doc["value"] | -1;
        }
    }

    if (vol < 0 || vol > 100) return sendJsonError(req, 400, "Volume must be between 0 and 100");

    EmbeddedSysDb::getInstance().mutate([vol](SystemState& s) {
        s.audio.speaker_volume = vol;
    });

    JsonDocument doc;
    doc["status"] = "ok";
    doc["speaker_volume"] = vol;
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::audioMicGainHandler(httpd_req_t* req) {
    std::string val_str;
    float gain = -1.0f;
    if (getQueryParam(req, "value", val_str)) {
        gain = static_cast<float>(atof(val_str.c_str()));
    } else if (req->content_len > 0) {
        std::string body(req->content_len + 1, '\0');
        httpd_req_recv(req, &body[0], req->content_len);
        JsonDocument doc;
        if (!deserializeJson(doc, body.c_str())) {
            gain = doc["gain"] | doc["value"] | -1.0f;
        }
    }

    if (gain < 0.0f || gain > 60.0f) return sendJsonError(req, 400, "Mic gain must be between 0 and 60.0 dB");

    EmbeddedSysDb::getInstance().mutate([gain](SystemState& s) {
        s.audio.mic_gain_db = gain;
    });

    JsonDocument doc;
    doc["status"] = "ok";
    doc["mic_gain_db"] = gain;
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::audioMicMuteHandler(httpd_req_t* req) {
    std::string val_str;
    bool muted = false;
    if (getQueryParam(req, "muted", val_str)) {
        muted = (val_str == "1" || val_str == "true");
    } else if (req->content_len > 0) {
        std::string body(req->content_len + 1, '\0');
        httpd_req_recv(req, &body[0], req->content_len);
        JsonDocument doc;
        if (!deserializeJson(doc, body.c_str())) {
            muted = doc["muted"] | false;
        }
    }

    EmbeddedSysDb::getInstance().mutate([muted](SystemState& s) {
        s.audio.mic_enabled = !muted;
    });

    JsonDocument doc;
    doc["status"] = "ok";
    doc["mic_enabled"] = !muted;
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::audioAlertHandler(httpd_req_t* req) {
    AlertPlayer::getInstance().playAlert(ALERT_WAKE_CONFIRM);
    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Alert chime triggered";
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::audioRecordHandler(httpd_req_t* req) {
    std::string uri = req->uri;
    bool is_start = (uri.find("/start") != std::string::npos);

    if (is_start) {
        if (!AudioRecorder::getInstance().startRecording(AudioRecorder::RecordMode::RAW)) {
            return sendJsonError(req, 409, "Recording is already active");
        }
    } else {
        AudioRecorder::getInstance().stopRecording(AudioRecorder::StopReason::MANUAL);
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["is_recording"] = AudioRecorder::getInstance().isRecording();
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

// ─────────────────────────────────────────────────────────────────────────────
// LED Lighting Handlers
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t HttpFileServerService::ledSetHandler(httpd_req_t* req) {
    if (req->content_len == 0 || req->content_len > 512) {
        return sendJsonError(req, 400, "Missing JSON payload");
    }

    std::string body(req->content_len + 1, '\0');
    httpd_req_recv(req, &body[0], req->content_len);

    JsonDocument doc;
    if (deserializeJson(doc, body.c_str())) {
        return sendJsonError(req, 400, "Invalid JSON payload");
    }

    const char* mode_str = doc["mode"] | "solid";
    int r = doc["r"] | 0;
    int g = doc["g"] | 0;
    int b = doc["b"] | 0;
    uint32_t speed = doc["speed_ms"] | 500;

    LedMode mode = LedMode::SOLID;
    if (strcasecmp(mode_str, "breath") == 0) mode = LedMode::BREATH;
    else if (strcasecmp(mode_str, "rainbow") == 0) mode = LedMode::RAINBOW;
    else if (strcasecmp(mode_str, "blink") == 0) mode = LedMode::BLINK;
    else if (strcasecmp(mode_str, "off") == 0) mode = LedMode::OFF;

    EmbeddedSysDb::getInstance().mutate([mode, r, g, b, speed](SystemState& s) {
        s.led.mode = mode;
        // Hardware WS2812 is wired GRB: struct fields {r, g, b} map to hardware {G, R, B}
        s.led.color = RgbColor{ static_cast<uint8_t>(g), static_cast<uint8_t>(r), static_cast<uint8_t>(b) };
        s.led.speed_ms = speed;
    });

    JsonDocument resp;
    resp["status"] = "ok";
    resp["mode"] = mode_str;
    std::string out;
    serializeJson(resp, out);
    return sendJsonResponse(req, 200, out);
}

// ─────────────────────────────────────────────────────────────────────────────
// Music & Library Handlers
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t HttpFileServerService::musicPlayHandler(httpd_req_t* req) {
    if (req->content_len <= 0 || req->content_len > 8192) {
        return sendJsonError(req, 400, "Invalid payload size");
    }

    std::string body(req->content_len + 1, '\0');
    int received = httpd_req_recv(req, &body[0], req->content_len);
    if (received <= 0) {
        return sendJsonError(req, 400, "Failed to read request body");
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body.c_str());
    if (err) {
        return sendJsonError(req, 400, "Invalid JSON payload");
    }

    const char* stream_url = doc["stream_url"];
    if (!stream_url || strlen(stream_url) == 0) {
        return sendJsonError(req, 400, "stream_url is required");
    }

    std::string id = doc["id"] | doc["videoId"] | "";
    std::string title = doc["title"] | "Unknown Title";
    std::string artist = doc["artist"] | doc["author"] | "Unknown Artist";
    int duration = doc["duration"] | doc["durationSeconds"] | 0;

    InvidiousTrack track;
    track.videoId = id;
    track.title = title;
    track.author = artist;
    track.durationSeconds = duration;

    bool ok = MusicPlaybackService::getInstance().playDirect(track, stream_url);
    if (!ok) {
        return sendJsonError(req, 500, "Failed to start direct playback");
    }

    JsonDocument resp;
    resp["status"] = "ok";
    resp["message"] = "Playback started";
    resp["id"] = id;
    resp["title"] = title;
    std::string out;
    serializeJson(resp, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::musicPlayLocalHandler(httpd_req_t* req) {
    std::string id_or_path;
    if (!getQueryParam(req, "id", id_or_path) && !getQueryParam(req, "path", id_or_path)) {
        if (req->content_len > 0 && req->content_len < 2048) {
            std::string body(req->content_len + 1, '\0');
            httpd_req_recv(req, &body[0], req->content_len);
            JsonDocument doc;
            if (!deserializeJson(doc, body.c_str())) {
                const char* id = doc["id"];
                const char* path = doc["path"];
                if (id) id_or_path = id;
                else if (path) id_or_path = path;
            }
        }
    }

    if (id_or_path.empty()) {
        return sendJsonError(req, 400, "Missing id or path parameter");
    }

    bool ok = MusicPlaybackService::getInstance().playLocal(id_or_path.c_str());
    if (!ok) {
        return sendJsonError(req, 404, "Track file not found or failed to play");
    }

    JsonDocument resp;
    resp["status"] = "ok";
    resp["target"] = id_or_path;
    std::string out;
    serializeJson(resp, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::musicControlHandler(httpd_req_t* req) {
    std::string action;
    int int_val = 0;
    bool bool_val = false;

    if (getQueryParam(req, "action", action)) {
        std::string val_str;
        if (getQueryParam(req, "value", val_str)) {
            int_val = atoi(val_str.c_str());
            bool_val = (val_str == "1" || val_str == "true");
        }
    } else if (req->content_len > 0 && req->content_len < 2048) {
        std::string body(req->content_len + 1, '\0');
        httpd_req_recv(req, &body[0], req->content_len);
        JsonDocument doc;
        if (!deserializeJson(doc, body.c_str())) {
            const char* a = doc["action"];
            if (a) action = a;
            if (doc["value"].is<int>()) {
                int_val = doc["value"].as<int>();
            } else if (doc["value"].is<bool>()) {
                bool_val = doc["value"].as<bool>();
            }
        }
    }

    if (action.empty()) {
        return sendJsonError(req, 400, "Missing action parameter");
    }

    if (action == "pause") {
        MusicPlaybackService::getInstance().pause();
    } else if (action == "resume") {
        MusicPlaybackService::getInstance().resume();
    } else if (action == "toggle") {
        MusicPlaybackService::getInstance().postCommand(MediaCmdType::TOGGLE_PLAY_PAUSE);
    } else if (action == "next") {
        MusicPlaybackService::getInstance().next();
    } else if (action == "prev" || action == "previous") {
        MusicPlaybackService::getInstance().previous();
    } else if (action == "stop") {
        MusicPlaybackService::getInstance().stop();
    } else if (action == "clear_queue") {
        MusicPlaybackService::getInstance().clearQueue();
    } else if (action == "shuffle") {
        MusicPlaybackService::getInstance().shuffleQueue();
    } else if (action == "repeat") {
        RepeatMode m = static_cast<RepeatMode>(int_val);
        MusicPlaybackService::getInstance().setRepeatMode(m);
    } else if (action == "autoplay") {
        MusicPlaybackService::getInstance().setAutoplay(bool_val);
    } else if (action == "caching") {
        MusicPlaybackService::getInstance().setCaching(bool_val);
    } else if (action == "seek") {
        MusicPlaybackService::getInstance().seekTo(static_cast<uint32_t>(int_val));
    } else {
        return sendJsonError(req, 400, "Unknown action");
    }

    JsonDocument resp;
    resp["status"] = "ok";
    resp["action"] = action;
    std::string out;
    serializeJson(resp, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::musicStatusHandler(httpd_req_t* req) {
    PlayerState pState = NexusPlayer::getInstance().getState();
    const char* state_str = "IDLE";
    switch (pState) {
        case STATE_STREAMING_AND_CACHING: state_str = "STREAMING"; break;
        case STATE_LOCAL_PLAYBACK:        state_str = "LOCAL"; break;
        case STATE_PAUSED:                state_str = "PAUSED"; break;
        default:                          state_str = "IDLE"; break;
    }

    InvidiousTrack cur = MusicPlaybackService::getInstance().getCurrentTrack();
    auto q = MusicPlaybackService::getInstance().getQueue();
    uint32_t pos_ms = MusicPlaybackService::getInstance().getPositionMs();
    uint32_t dur_ms = cur.durationSeconds * 1000;
    bool seekable = (pState == STATE_LOCAL_PLAYBACK || pState == STATE_STREAMING_AND_CACHING || pState == STATE_PAUSED);

    JsonDocument doc;
    doc["state"] = state_str;
    doc["position_ms"] = pos_ms;
    doc["duration_ms"] = dur_ms;
    doc["seekable"] = seekable;
    JsonObject t = doc["current_track"].to<JsonObject>();
    t["id"] = cur.videoId;
    t["title"] = cur.title;
    t["artist"] = cur.author;
    t["duration"] = cur.durationSeconds;

    doc["repeat_mode"] = static_cast<int>(MusicPlaybackService::getInstance().getRepeatMode());
    doc["autoplay"] = MusicPlaybackService::getInstance().isAutoplayEnabled();
    doc["caching"] = MusicPlaybackService::getInstance().isCachingEnabled();
    doc["queue_count"] = q.size();

    JsonArray qa = doc["queue"].to<JsonArray>();
    int count = 0;
    for (const auto& item : q) {
        if (++count > 10) break;
        JsonObject obj = qa.add<JsonObject>();
        obj["id"] = item.videoId;
        obj["title"] = item.title;
        obj["artist"] = item.author;
    }

    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::musicLibraryHandler(httpd_req_t* req) {
    std::string filter;
    getQueryParam(req, "q", filter);

    std::string json = CatalogDB::getInstance().serializeLibraryJson(filter);
    return sendJsonResponse(req, 200, json);
}

esp_err_t HttpFileServerService::musicLibraryScanHandler(httpd_req_t* req) {
    size_t count = CatalogDB::getInstance().scanAndSync();
    JsonDocument doc;
    doc["status"] = "ok";
    doc["scanned_count"] = count;
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::musicLibraryDeleteHandler(httpd_req_t* req) {
    std::string id;
    if (!getQueryParam(req, "id", id) || id.empty()) {
        if (req->content_len > 0 && req->content_len < 512) {
            std::string body(req->content_len + 1, '\0');
            httpd_req_recv(req, &body[0], req->content_len);
            JsonDocument doc;
            if (!deserializeJson(doc, body.c_str())) {
                const char* id_field = doc["id"];
                if (id_field) id = id_field;
            }
        }
    }

    if (id.empty()) {
        return sendJsonError(req, 400, "Missing id parameter");
    }

    bool ok = CatalogDB::getInstance().remove(id.c_str());
    JsonDocument doc;
    doc["status"] = ok ? "ok" : "error";
    doc["id"] = id;
    if (!ok) doc["message"] = "Track not found in library";
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, ok ? 200 : 404, out);
}

// ─────────────────────────────────────────────────────────────────────────────
// Real-time CPU Usage & Telemetry Handlers
// ─────────────────────────────────────────────────────────────────────────────

void HttpFileServerService::getCpuUsage(int& cpu0_pct, int& cpu1_pct) {
#if (configGENERATE_RUN_TIME_STATS == 1)
    static uint32_t s_last_idle0 = 0;
    static uint32_t s_last_idle1 = 0;
    static int64_t  s_last_time_us = 0;
    static int      s_cached_cpu0 = 0;
    static int      s_cached_cpu1 = 0;

    int64_t now_us = esp_timer_get_time();
    uint32_t idle0 = ulTaskGetIdleRunTimeCounterForCore(0);
    uint32_t idle1 = ulTaskGetIdleRunTimeCounterForCore(1);

    if (s_last_time_us > 0) {
        int64_t dt_us = now_us - s_last_time_us;
        if (dt_us >= 250000) { // Refresh at least every 250ms
            float idle0_fraction = static_cast<float>(idle0 - s_last_idle0) / static_cast<float>(dt_us);
            float idle1_fraction = static_cast<float>(idle1 - s_last_idle1) / static_cast<float>(dt_us);

            if (idle0_fraction > 1.0f) idle0_fraction = 1.0f;
            if (idle0_fraction < 0.0f) idle0_fraction = 0.0f;
            if (idle1_fraction > 1.0f) idle1_fraction = 1.0f;
            if (idle1_fraction < 0.0f) idle1_fraction = 0.0f;

            s_cached_cpu0 = static_cast<int>((1.0f - idle0_fraction) * 100.0f);
            s_cached_cpu1 = static_cast<int>((1.0f - idle1_fraction) * 100.0f);

            s_last_idle0 = idle0;
            s_last_idle1 = idle1;
            s_last_time_us = now_us;
        }
    } else {
        s_last_idle0 = idle0;
        s_last_idle1 = idle1;
        s_last_time_us = now_us;
    }

    cpu0_pct = s_cached_cpu0;
    cpu1_pct = s_cached_cpu1;
#else
    cpu0_pct = 0;
    cpu1_pct = 0;
#endif
}

esp_err_t HttpFileServerService::metricsHandler(httpd_req_t* req) {
    JsonDocument doc;

    // 1. System Overview & Realtime CPU%
    doc["uptime_sec"] = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
    doc["num_tasks"] = uxTaskGetNumberOfTasks();
    int cpu0 = 0, cpu1 = 0;
    getCpuUsage(cpu0, cpu1);
    doc["cpu0"] = cpu0;
    doc["cpu1"] = cpu1;

    const char* reset_desc = "Unknown";
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   reset_desc = "Power-on Reset"; break;
        case ESP_RST_SW:        reset_desc = "Software Reset"; break;
        case ESP_RST_PANIC:     reset_desc = "Software Panic / Crash"; break;
        case ESP_RST_INT_WDT:   reset_desc = "Interrupt Watchdog"; break;
        case ESP_RST_TASK_WDT:  reset_desc = "Task Watchdog"; break;
        case ESP_RST_BROWNOUT:  reset_desc = "Brownout (Voltage Drop)"; break;
        default:                reset_desc = "Normal Boot"; break;
    }
    doc["reset_reason"] = reset_desc;

    // 2. Memory Heaps
    JsonObject heap = doc["heap"].to<JsonObject>();
    heap["internal_free"]     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    heap["internal_total"]    = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    heap["internal_min_free"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    heap["psram_free"]        = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    heap["psram_total"]       = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    // 3. Wi-Fi Station
    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        wifi["connected"] = true;
        wifi["ssid"] = reinterpret_cast<const char*>(ap_info.ssid);
        wifi["rssi"] = ap_info.rssi;
        wifi["channel"] = ap_info.primary;
    } else {
        wifi["connected"] = false;
        wifi["rssi"] = 0;
    }

    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char ip_str[16];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            wifi["ip"] = ip_str;
        }
    }

    // 4. Audio & Pipeline State
    SystemState snap = EmbeddedSysDb::getInstance().snapshot();
    JsonObject audio = doc["audio"].to<JsonObject>();
    audio["speaker_volume"] = snap.audio.speaker_volume;
    audio["mic_gain_db"]    = snap.audio.mic_gain_db;
    audio["mic_enabled"]    = snap.audio.mic_enabled;
    audio["sample_rate"]    = snap.audio.sample_rate;
    audio["is_recording"]   = AudioRecorder::getInstance().isRecording();

    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::systemInitHandler(httpd_req_t* req) {
    JsonDocument doc;
    doc["board"] = "ESP32-S3 (Waveshare)";
    const esp_app_desc_t* app_desc = esp_app_get_description();
    doc["version"]      = app_desc ? app_desc->version : "unknown";
    doc["compile_date"] = app_desc ? app_desc->date : "unknown";
    doc["compile_time"] = app_desc ? app_desc->time : "unknown";

    const char* reset_desc = "Unknown";
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   reset_desc = "Power-on Reset"; break;
        case ESP_RST_SW:        reset_desc = "Software Reset"; break;
        case ESP_RST_PANIC:     reset_desc = "Software Panic / Crash"; break;
        case ESP_RST_INT_WDT:   reset_desc = "Interrupt Watchdog"; break;
        case ESP_RST_TASK_WDT:  reset_desc = "Task Watchdog"; break;
        case ESP_RST_BROWNOUT:  reset_desc = "Brownout (Voltage Drop)"; break;
        default:                reset_desc = "Normal Boot"; break;
    }
    doc["reset_reason"] = reset_desc;

    doc["internal_total"] = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    doc["psram_total"]    = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* target  = esp_ota_get_next_update_partition(nullptr);
    doc["running_partition"] = running ? running->label : "unknown";
    doc["target_partition"]  = target ? target->label : "unknown";

    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        doc["wifi_ssid"]    = reinterpret_cast<const char*>(ap_info.ssid);
        doc["wifi_channel"] = ap_info.primary;
    }
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char ip_str[16];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            doc["ip"] = ip_str;
        }
    }

    SystemState snap = EmbeddedSysDb::getInstance().snapshot();
    JsonObject state = doc["state"].to<JsonObject>();
    state["speaker_volume"] = snap.audio.speaker_volume;
    state["mic_gain_db"]    = snap.audio.mic_gain_db;
    state["mic_enabled"]    = snap.audio.mic_enabled;
    state["sample_rate"]    = snap.audio.sample_rate;
    state["is_recording"]   = AudioRecorder::getInstance().isRecording();
    state["led_mode"]       = static_cast<int>(snap.led.mode);
    state["led_r"]          = snap.led.color.r;
    state["led_g"]          = snap.led.color.g;
    state["led_b"]          = snap.led.color.b;

    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::systemDeltaHandler(httpd_req_t* req) {
    JsonDocument doc;

    // 1. Dynamic Uptime & Realtime CPU%
    doc["up"] = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
    int cpu0 = 0, cpu1 = 0;
    getCpuUsage(cpu0, cpu1);
    doc["c0"] = cpu0;
    doc["c1"] = cpu1;

    // 2. High-churn Dynamic Memory
    doc["sram"]     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    doc["min_sram"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    doc["psram"]    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    // 3. Wi-Fi RSSI
    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        doc["rssi"] = ap_info.rssi;
    } else {
        doc["rssi"] = 0;
    }

    // 4. State Delta: check if control state changed since last request
    SystemState snap = EmbeddedSysDb::getInstance().snapshot();
    static int   s_last_vol = -1;
    static float s_last_gain = -1.0f;
    static bool  s_last_mic = false;
    static bool  s_last_rec = false;
    bool is_rec = AudioRecorder::getInstance().isRecording();

    if (snap.audio.speaker_volume != s_last_vol ||
        snap.audio.mic_gain_db != s_last_gain ||
        snap.audio.mic_enabled != s_last_mic ||
        is_rec != s_last_rec) {
        s_last_vol  = snap.audio.speaker_volume;
        s_last_gain = snap.audio.mic_gain_db;
        s_last_mic  = snap.audio.mic_enabled;
        s_last_rec  = is_rec;

        JsonObject state = doc["state"].to<JsonObject>();
        state["speaker_volume"] = s_last_vol;
        state["mic_gain_db"]    = s_last_gain;
        state["mic_enabled"]    = s_last_mic;
        state["is_recording"]   = s_last_rec;
    }

    // 5. Delta Logs: fetch only logs since client's last seen sequence number
    std::string seq_str;
    uint32_t since_seq = 0;
    if (getQueryParam(req, "log_seq", seq_str)) {
        since_seq = static_cast<uint32_t>(strtoul(seq_str.c_str(), nullptr, 10));
    }
    std::vector<std::pair<uint32_t, std::string>> logs;
    uint32_t latest_seq = 0;
    LogRouter::getInstance().getLogsSince(since_seq, logs, latest_seq);
    doc["latest_seq"] = latest_seq;
    if (!logs.empty()) {
        JsonArray log_arr = doc["logs"].to<JsonArray>();
        for (const auto& l : logs) {
            log_arr.add(l.second);
        }
    }

    // 6. Music Playback State
    PlayerState pState = NexusPlayer::getInstance().getState();
    const char* mstate_str = "IDLE";
    switch (pState) {
        case STATE_STREAMING_AND_CACHING: mstate_str = "STREAMING"; break;
        case STATE_LOCAL_PLAYBACK:        mstate_str = "LOCAL"; break;
        case STATE_PAUSED:                mstate_str = "PAUSED"; break;
        default:                          mstate_str = "IDLE"; break;
    }
    JsonObject mobj = doc["music"].to<JsonObject>();
    mobj["state"] = mstate_str;
    InvidiousTrack cur = MusicPlaybackService::getInstance().getCurrentTrack();
    JsonObject t = mobj["current_track"].to<JsonObject>();
    t["id"] = cur.videoId;
    t["title"] = cur.title;
    t["artist"] = cur.author;
    t["duration"] = cur.durationSeconds;
    mobj["position_ms"] = MusicPlaybackService::getInstance().getPositionMs();
    mobj["duration_ms"] = cur.durationSeconds * 1000;
    mobj["seekable"] = (pState == STATE_LOCAL_PLAYBACK || pState == STATE_STREAMING_AND_CACHING || pState == STATE_PAUSED);
    mobj["repeat_mode"] = static_cast<int>(MusicPlaybackService::getInstance().getRepeatMode());
    mobj["autoplay"] = MusicPlaybackService::getInstance().isAutoplayEnabled();
    mobj["caching"] = MusicPlaybackService::getInstance().isCachingEnabled();

    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

// ─────────────────────────────────────────────────────────────────────────────
// Config Management Handlers
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t HttpFileServerService::configGetHandler(httpd_req_t* req) {
    std::string uri = req->uri;
    const char* path = (uri.find("gemini") != std::string::npos) ? "/sdcard/gemini_config.json" : "/sdcard/settings.txt";

    std::string content = StorageService::getInstance().readFile(path);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, (uri.find("gemini") != std::string::npos) ? "application/json" : "text/plain");
    return httpd_resp_send(req, content.c_str(), content.length());
}

esp_err_t HttpFileServerService::configSetHandler(httpd_req_t* req) {
    std::string uri = req->uri;
    const char* path = (uri.find("gemini") != std::string::npos) ? "/sdcard/gemini_config.json" : "/sdcard/settings.txt";

    std::string body(req->content_len + 1, '\0');
    int received = httpd_req_recv(req, &body[0], req->content_len);
    if (received < 0) return sendJsonError(req, 500, "Socket receive failed");

    if (!StorageService::getInstance().writeFile(path, body.c_str())) {
        return sendJsonError(req, 500, "Failed to write config file to SD");
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Config updated successfully";
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

// ─────────────────────────────────────────────────────────────────────────────
// Live Console Logs Handler
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t HttpFileServerService::logsHandler(httpd_req_t* req) {
    std::string since_str;
    uint32_t since_seq = 0;
    if (getQueryParam(req, "since", since_str)) {
        since_seq = static_cast<uint32_t>(strtoul(since_str.c_str(), nullptr, 10));
    }

    std::vector<std::pair<uint32_t, std::string>> logs;
    uint32_t latest_seq = 0;
    LogRouter::getInstance().getLogsSince(since_seq, logs, latest_seq);

    JsonDocument doc;
    doc["latest_seq"] = latest_seq;
    JsonArray arr = doc["logs"].to<JsonArray>();
    for (const auto& l : logs) {
        arr.add(l.second);
    }

    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

// ─────────────────────────────────────────────────────────────────────────────
// OTA Web Flasher Handlers
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t HttpFileServerService::otaStatusHandler(httpd_req_t* req) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    const esp_app_desc_t* app_desc = esp_app_get_description();

    JsonDocument doc;
    doc["running_partition"] = running ? running->label : "unknown";
    doc["target_partition"]  = target ? target->label : "unknown";
    doc["version"]           = app_desc ? app_desc->version : "unknown";
    doc["compile_date"]      = app_desc ? app_desc->date : "unknown";
    doc["compile_time"]      = app_desc ? app_desc->time : "unknown";

    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

namespace {

enum OtaCmd {
    OTA_CMD_NONE,
    OTA_CMD_BEGIN,
    OTA_CMD_WRITE,
    OTA_CMD_END,
    OTA_CMD_ABORT
};

struct OtaWorkState {
    OtaCmd cmd = OTA_CMD_NONE;
    const esp_partition_t* partition = nullptr;
    const char* buf = nullptr;
    size_t length = 0;
    esp_err_t result = ESP_OK;
    esp_ota_handle_t ota_handle = 0;
    SemaphoreHandle_t req_sem = nullptr;
    SemaphoreHandle_t done_sem = nullptr;
    volatile bool running = false;
};

// Static task control block, stack, and chunk buffer strictly in internal SRAM (.bss).
// Guarantees s_task_stack_is_sane_when_cache_frozen() passes without allocating from heap!
static StaticTask_t s_ota_tcb;
static StackType_t  s_ota_stack[3072]; // 12288 bytes (12 KB in internal SRAM, bulletproof for esp_image validation & SHA-256)
static char         s_ota_chunk[4096]; // 4096 bytes (4 KB aligned to flash sector size for 2x faster flashing)

static void otaWorkerThunk(void* arg) {
    auto* s = static_cast<OtaWorkState*>(arg);
    while (s->running) {
        if (xSemaphoreTake(s->req_sem, portMAX_DELAY) != pdTRUE) break;
        if (!s->running) break;

        switch (s->cmd) {
            case OTA_CMD_BEGIN:
                s->result = esp_ota_begin(s->partition, OTA_WITH_SEQUENTIAL_WRITES, &s->ota_handle);
                break;
            case OTA_CMD_WRITE:
                s->result = esp_ota_write(s->ota_handle, s->buf, s->length);
                break;
            case OTA_CMD_END:
                s->result = esp_ota_end(s->ota_handle);
                if (s->result == ESP_OK) {
                    s->result = esp_ota_set_boot_partition(s->partition);
                }
                break;
            case OTA_CMD_ABORT:
                if (s->ota_handle != 0) {
                    esp_ota_abort(s->ota_handle);
                    s->ota_handle = 0;
                }
                s->result = ESP_OK;
                break;
            default:
                s->result = ESP_OK;
                break;
        }

        xSemaphoreGive(s->done_sem);
    }
    vTaskDelete(nullptr);
}

} // namespace

esp_err_t HttpFileServerService::otaUploadHandler(httpd_req_t* req) {
    if (req->content_len <= 0) {
        return sendJsonError(req, 400, "Empty payload for OTA update");
    }

    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(nullptr);
    if (!update_partition) {
        return sendJsonError(req, 500, "No OTA partition available for flashing");
    }

    ESP_LOGI(TAG, "Starting OTA update to partition '%s' (size %ld bytes)...", update_partition->label, req->content_len);

    OtaWorkState state;
    state.partition = update_partition;
    state.req_sem = xSemaphoreCreateBinary();
    state.done_sem = xSemaphoreCreateBinary();
    state.running = true;

    TaskHandle_t worker = xTaskCreateStatic(
        otaWorkerThunk,
        "ota_flasher",
        sizeof(s_ota_stack) / sizeof(s_ota_stack[0]),
        &state,
        ThreadConfig::Priority::LOW + 1,
        s_ota_stack,
        &s_ota_tcb
    );

    if (!worker) {
        vSemaphoreDelete(state.req_sem);
        vSemaphoreDelete(state.done_sem);
        return sendJsonError(req, 500, "Failed to create internal OTA worker");
    }

    // 1. Begin OTA inside internal-stack worker
    state.cmd = OTA_CMD_BEGIN;
    xSemaphoreGive(state.req_sem);
    xSemaphoreTake(state.done_sem, portMAX_DELAY);

    if (state.result != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(state.result));
        state.running = false;
        xSemaphoreGive(state.req_sem);
        vSemaphoreDelete(state.req_sem);
        vSemaphoreDelete(state.done_sem);
        return sendJsonError(req, 500, "esp_ota_begin failed");
    }

    // 2. Stream chunks through internal SRAM buffer
    constexpr size_t CHUNK_SIZE = sizeof(s_ota_chunk);
    int remaining = req->content_len;
    bool success = true;

    while (remaining > 0) {
        int to_read = (remaining < static_cast<int>(CHUNK_SIZE)) ? remaining : static_cast<int>(CHUNK_SIZE);
        int received = httpd_req_recv(req, s_ota_chunk, to_read);

        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "OTA socket transfer interrupted");
            success = false;
            break;
        }

        state.cmd = OTA_CMD_WRITE;
        state.buf = s_ota_chunk;
        state.length = received;
        xSemaphoreGive(state.req_sem);
        xSemaphoreTake(state.done_sem, portMAX_DELAY);

        if (state.result != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(state.result));
            success = false;
            break;
        }

        remaining -= received;
    }

    // 3. Finalize or abort
    if (success) {
        state.cmd = OTA_CMD_END;
        xSemaphoreGive(state.req_sem);
        xSemaphoreTake(state.done_sem, portMAX_DELAY);
        if (state.result != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end / set boot partition failed: %s", esp_err_to_name(state.result));
            success = false;
        }
    } else {
        state.cmd = OTA_CMD_ABORT;
        xSemaphoreGive(state.req_sem);
        xSemaphoreTake(state.done_sem, portMAX_DELAY);
    }

    state.running = false;
    xSemaphoreGive(state.req_sem);
    vTaskDelay(pdMS_TO_TICKS(50));
    vSemaphoreDelete(state.req_sem);
    vSemaphoreDelete(state.done_sem);

    if (!success) {
        return sendJsonError(req, 500, "OTA Flashing failed");
    }

    ESP_LOGI(TAG, "OTA Flashing complete! Scheduled restart in 2 seconds...");

    static esp_timer_handle_t s_ota_reboot_timer = nullptr;
    if (!s_ota_reboot_timer) {
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = [](void*) { esp_restart(); };
        timer_args.arg = nullptr;
        timer_args.dispatch_method = ESP_TIMER_TASK;
        timer_args.name = "ota_reboot";
        esp_timer_create(&timer_args, &s_ota_reboot_timer);
    }
    esp_timer_start_once(s_ota_reboot_timer, 2000000ULL);

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "OTA Flash successful! Device rebooting...";
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::systemRebootHandler(httpd_req_t* req) {
    static esp_timer_handle_t s_web_reboot_timer = nullptr;
    if (!s_web_reboot_timer) {
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = [](void*) { esp_restart(); };
        timer_args.arg = nullptr;
        timer_args.dispatch_method = ESP_TIMER_TASK;
        timer_args.name = "web_reboot";
        esp_timer_create(&timer_args, &s_web_reboot_timer);
    }
    esp_timer_start_once(s_web_reboot_timer, 1500000ULL);

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Rebooting device...";
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

} // namespace Services
