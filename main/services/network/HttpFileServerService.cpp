#include "HttpFileServerService.h"
#include "WebDashboardHtml.h"
#include "services/storage/StorageService.h"
#include "common/AppLogger.h"
#include "common/thread_config.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "core_sysdb/LogRouter.h"
#include "core_sysdb/led_types.h"
#include "app/audio/recording/AudioRecorder.h"
#include "audio_core/AlertPlayer.h"

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
#include <sys/stat.h>
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
    ESP_LOGI(TAG, "HttpFileServerService initialized, awaiting Wi-Fi connection...");
    return true;
}

void HttpFileServerService::onStateChanged(ComponentMask changed, const SystemState& snap) {
    if (!(changed & COMP::SYSTEM)) {
        return;
    }

    bool wifi_connected = snap.system.wifi_connected;
    if (wifi_connected && !m_wifi_was_connected) {
        m_wifi_was_connected = true;
        ESP_LOGI(TAG, "Wi-Fi connected — starting Control Hub & HTTP server on port %d...", CONFIG_WAVESHARE_HTTP_FILE_SERVER_PORT);
        startServer();
    } else if (!wifi_connected && m_wifi_was_connected) {
        m_wifi_was_connected = false;
        ESP_LOGI(TAG, "Wi-Fi disconnected — stopping Control Hub server...");
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
    config.task_caps = MALLOC_CAP_SPIRAM; // Keeps 100% of internal SRAM free for audio/Wi-Fi!
    config.stack_size = 12288;
    config.core_id = ThreadConfig::CORE_NETWORK;
    config.max_uri_handlers = 48;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;

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
            .user_ctx = nullptr
        };
        httpd_register_uri_handler(m_server, &u);
    };

    // 1. Dashboard Web UI
    reg("/", HTTP_GET, indexHandler);
    reg("/index.html", HTTP_GET, indexHandler);

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

    // 4. LED Lighting Controls
    reg("/api/led/set", HTTP_POST, ledSetHandler);

    // 5. System Metrics & Telemetry
    reg("/api/system/metrics", HTTP_GET, metricsHandler);

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
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, WEB_DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t HttpFileServerService::optionsHandler(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
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
// Telemetry & Metrics Handler
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t HttpFileServerService::metricsHandler(httpd_req_t* req) {
    JsonDocument doc;

    // 1. System Overview
    doc["uptime_sec"] = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
    doc["num_tasks"] = uxTaskGetNumberOfTasks();

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
static StackType_t  s_ota_stack[2048]; // 8192 bytes (in internal SRAM, accommodates esp_ota_end SHA-256 verification)
static char         s_ota_chunk[2048]; // 2048 bytes (in internal SRAM)

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

    xTaskCreate([](void*) {
        vTaskDelay(pdMS_TO_TICKS(2500));
        esp_restart();
    }, "ota_reboot", 2048, nullptr, 5, nullptr);

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "OTA Flash successful! Device rebooting...";
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

esp_err_t HttpFileServerService::systemRebootHandler(httpd_req_t* req) {
    xTaskCreate([](void*) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }, "web_reboot", 2048, nullptr, 5, nullptr);

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Rebooting device...";
    std::string out;
    serializeJson(doc, out);
    return sendJsonResponse(req, 200, out);
}

} // namespace Services
