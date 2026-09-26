// /api/alerts: alert chimes (source, gain, enable, upload, preview). Settings
// live in the "alerts" collection of system.ndb; AlertLibrary applies them.
//
//   GET  /api/alerts                   every alert, the files in the alert folder, limits
//   POST /api/alerts/<name>            {"enabled"?, "gain_db"?, "source"?}
//   POST /api/alerts/<name>/upload     raw audio body; ?file=<name> to choose the file name
//   POST /api/alerts/<name>/play       preview, even when disabled
//   POST /api/alerts/<name>/reset      back to the default
#include "services/http/routes/Routes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>

#include "audio_core/AlertPlayer.h"
#include "esp_heap_caps.h"
#include "http_server/HttpUtil.h"
#include "sd_storage/File.h"
#include "sd_storage/Fs.h"
#include "sd_storage/SdCard.h"
#include "services/alerts/AlertLibrary.h"
#include "services/storage/SystemDatabase.h"

namespace {

using Services::AlertLibrary;
using ndb::system::AlertConfig;

constexpr uint32_t DECODE_TIMEOUT_MS = 10000;
constexpr size_t CHUNK_SIZE = 4096;
constexpr const char* UPLOAD_TMP = "/sdcard/media/alert/.upload.tmp";

std::string alertPath(const std::string& file) {
    return std::string(AlertLibrary::ALERT_DIR) + "/" + file;
}

bool hasAudioExtension(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    for (const char* ext : {".ogg", ".opus", ".webm"}) {
        size_t n = strlen(ext);
        if (lower.size() > n && lower.compare(lower.size() - n, n, ext) == 0) return true;
    }
    return false;
}

void addAlert(JsonObject o, AlertType type) {
    AlertLibrary::Status st = AlertLibrary::getInstance().status(type);
    o["name"] = alertName(type);
    o["enabled"] = st.enabled;
    o["gain_db"] = st.gain_db;
    o["source"] = st.setting;
    o["playing"] = st.file.empty() ? AlertLibrary::BUILTIN : st.file.c_str();
    o["custom"] = st.custom;
    o["duration_ms"] = AlertPlayer::getInstance().clipMs(type);
    if (!st.file.empty()) o["decode_ms"] = st.decode_ms;
    if (st.truncated) o["truncated"] = true;
    if (!st.error.empty()) o["error"] = st.error;
}

esp_err_t sendAlert(httpd_req_t* req, AlertType type, const char* message) {
    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = message;
    addAlert(doc["alert"].to<JsonObject>(), type);
    return Http::sendJson(req, 200, doc);
}

bool addFile(const sd_storage::DirEntry& e, void* ctx) {
    if (e.is_dir || e.name[0] == '.') return true;
    JsonObject f = static_cast<JsonArray*>(ctx)->add<JsonObject>();
    f["name"] = e.name;
    f["size"] = e.size;
    return true;
}

esp_err_t listHandler(httpd_req_t* req) {
    JsonDocument doc;
    JsonArray alerts = doc["alerts"].to<JsonArray>();
    for (int i = 0; i < ALERT_COUNT; ++i) addAlert(alerts.add<JsonObject>(), static_cast<AlertType>(i));
    JsonArray files = doc["files"].to<JsonArray>();
    if (sd_storage::SdCard::instance().isMounted()) {
        sd_storage::Fs::list(AlertLibrary::ALERT_DIR, nullptr, true, addFile, &files);
    }
    JsonObject limits = doc["limits"].to<JsonObject>();
    limits["max_seconds"] = AlertClip::MAX_SECONDS;
    limits["max_file_bytes"] = AlertLibrary::MAX_FILE_BYTES;
    limits["min_gain_db"] = AlertLibrary::MIN_GAIN_DB;
    limits["max_gain_db"] = AlertLibrary::MAX_GAIN_DB;
    return Http::sendJson(req, 200, doc);
}

esp_err_t updateAlert(httpd_req_t* req, AlertType type) {
    std::string body;
    JsonDocument doc;
    if (req->content_len == 0 || !Http::readBody(req, body, 1024) || deserializeJson(doc, body) ||
        !doc.is<JsonObject>()) {
        return Http::sendError(req, 400, "Body must be {\"enabled\"?, \"gain_db\"?, \"source\"?}");
    }

    AlertLibrary& lib = AlertLibrary::getInstance();
    AlertLibrary::Status st = lib.status(type);
    AlertConfig cfg;
    cfg.enabled = st.enabled;
    cfg.gain_db = st.gain_db;
    cfg.source = st.setting;
    uint64_t fields = 0;

    if (!doc["enabled"].isNull()) {
        if (!doc["enabled"].is<bool>()) return Http::sendError(req, 400, "enabled must be true or false");
        cfg.enabled = doc["enabled"].as<bool>();
        fields |= AlertConfig::F_ENABLED;
    }
    if (!doc["gain_db"].isNull()) {
        float g = doc["gain_db"].as<float>();
        if (!doc["gain_db"].is<float>() || !std::isfinite(g) || g < AlertLibrary::MIN_GAIN_DB ||
            g > AlertLibrary::MAX_GAIN_DB) {
            return Http::sendError(req, 400, "gain_db must be a number from -24 to 6");
        }
        cfg.gain_db = g;
        fields |= AlertConfig::F_GAIN_DB;
    }
    bool source_changed = false;
    if (!doc["source"].isNull()) {
        if (!doc["source"].is<const char*>()) return Http::sendError(req, 400, "source must be a string");
        std::string src = doc["source"].as<const char*>();
        if (!src.empty() && src != AlertLibrary::BUILTIN) {
            if (!AlertLibrary::isValidFileName(src)) return Http::sendError(req, 400, "source must be a file name in the alert folder");
            if (!sd_storage::Fs::isFile(alertPath(src).c_str())) return Http::sendError(req, 404, "No such file in the alert folder");
        }
        source_changed = src != cfg.source;
        cfg.source = src;
        fields |= AlertConfig::F_SOURCE;
    }
    if (!fields) return Http::sendError(req, 400, "Nothing to change");

    if (!Services::saveAlertConfig(alertName(type), cfg, fields)) {
        return Http::sendError(req, 503, "Settings database unavailable");
    }
    if (source_changed) {
        if (!lib.reloadAndWait(type, DECODE_TIMEOUT_MS)) return Http::sendError(req, 504, "Saved; still loading");
    } else {
        lib.applySettings(type, cfg.enabled, cfg.gain_db);
    }
    return sendAlert(req, type, "Alert updated");
}

esp_err_t uploadAlert(httpd_req_t* req, AlertType type) {
    if (!sd_storage::SdCard::instance().isMounted()) return Http::sendError(req, 503, "SD card not mounted");
    if (req->content_len == 0) return Http::sendError(req, 400, "Empty body; send the audio file as the request body");
    if (req->content_len > AlertLibrary::MAX_FILE_BYTES) return Http::sendError(req, 413, "File is over 256 KB");

    // Without ?file=, the name is <alert>-custom plus the extension of the
    // format sniffed from the first bytes.
    std::string file;
    Http::queryParam(req, "file", file);
    if (!file.empty() && (!AlertLibrary::isValidFileName(file) || !hasAudioExtension(file))) {
        return Http::sendError(req, 400, "file must be a plain .ogg, .opus or .webm file name");
    }

    sd_storage::Fs::mkdirs(AlertLibrary::ALERT_DIR);
    std::unique_ptr<char, decltype(&heap_caps_free)> chunk(
        static_cast<char*>(heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), &heap_caps_free);
    if (!chunk) return Http::sendError(req, 500, "Out of memory");
    {
        sd_storage::File f = sd_storage::File::open(UPLOAD_TMP, sd_storage::Mode::Write);
        if (!f) return Http::sendError(req, 500, "Cannot write to the alert folder");
        size_t remaining = req->content_len;
        while (remaining > 0) {
            int got = httpd_req_recv(req, chunk.get(), std::min(remaining, CHUNK_SIZE));
            if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (got <= 0 || !f.writeAll(chunk.get(), got)) {
                f.close();
                sd_storage::Fs::remove(UPLOAD_TMP);
                return Http::sendError(req, 500, "Upload failed");
            }
            if (file.empty()) {
                static const uint8_t EBML[4] = {0x1A, 0x45, 0xDF, 0xA3};
                bool webm = got >= 4 && memcmp(chunk.get(), EBML, 4) == 0;
                file = std::string(alertName(type)) + (webm ? "-custom.webm" : "-custom.ogg");
            }
            remaining -= got;
        }
    }

    // Decode before replacing anything, so a bad file changes nothing.
    Services::AlertDecode d;
    if (!AlertLibrary::getInstance().decodeAndWait(UPLOAD_TMP, d, DECODE_TIMEOUT_MS)) {
        return Http::sendError(req, 504, "Decoding timed out");
    }
    if (!d.clip) {
        sd_storage::Fs::remove(UPLOAD_TMP);
        std::string msg = "Not a playable audio file: " + d.error;
        return Http::sendError(req, 422, msg.c_str());
    }
    d.clip.reset();

    const std::string target = alertPath(file);
    if (sd_storage::Fs::exists(target.c_str()) && !sd_storage::Fs::remove(target.c_str())) {
        sd_storage::Fs::remove(UPLOAD_TMP);
        return Http::sendError(req, 409, "The existing file is busy");
    }
    if (!sd_storage::Fs::rename(UPLOAD_TMP, target.c_str())) {
        sd_storage::Fs::remove(UPLOAD_TMP);
        return Http::sendError(req, 500, "Failed to store the file");
    }

    AlertConfig cfg;
    cfg.source = file;
    if (!Services::saveAlertConfig(alertName(type), cfg, AlertConfig::F_SOURCE)) {
        return Http::sendError(req, 503, "File stored, but the settings database is unavailable");
    }
    if (!AlertLibrary::getInstance().reloadAndWait(type, DECODE_TIMEOUT_MS)) {
        return Http::sendError(req, 504, "Saved; still loading");
    }
    return sendAlert(req, type, "Alert uploaded");
}

esp_err_t alertHandler(httpd_req_t* req) {
    // /api/alerts/<name>[/<action>], query string ignored
    std::string rest(req->uri + strlen("/api/alerts/"));
    rest = rest.substr(0, rest.find('?'));
    size_t slash = rest.find('/');
    std::string name = rest.substr(0, slash);
    std::string action = slash == std::string::npos ? "" : rest.substr(slash + 1);

    AlertType type = alertFromName(name.c_str());
    if (type == ALERT_COUNT) return Http::sendError(req, 404, "Unknown alert");

    if (action.empty()) return updateAlert(req, type);
    if (action == "upload") return uploadAlert(req, type);
    if (action == "play") {
        AlertPlayer::getInstance().preview(type);
        return sendAlert(req, type, "Playing");
    }
    if (action == "reset") {
        if (!Services::resetAlertConfig(alertName(type))) {
            return Http::sendError(req, 503, "Settings database unavailable");
        }
        if (!AlertLibrary::getInstance().reloadAndWait(type, DECODE_TIMEOUT_MS)) {
            return Http::sendError(req, 504, "Reset; still loading");
        }
        return sendAlert(req, type, "Alert reset to default");
    }
    return Http::sendError(req, 404, "Unknown alert action");
}

} // namespace

void Routes::registerAlerts(Http::Server& server) {
    server.on("/api/alerts", HTTP_GET, listHandler);
    server.on("/api/alerts/*", HTTP_POST, alertHandler);
}
