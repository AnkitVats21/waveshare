#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "services/storage/StorageService.h"

using Services::StorageService;

namespace {

constexpr const char* GEMINI_CONFIG = "/sdcard/gemini_config.json";
constexpr const char* SETTINGS_FILE = "/sdcard/settings.txt";

// settings.txt: raw text passthrough.
esp_err_t getSettingsHandler(httpd_req_t* req) {
    std::string content = StorageService::getInstance().readFile(SETTINGS_FILE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, content.c_str(), content.length());
}

esp_err_t setSettingsHandler(httpd_req_t* req) {
    std::string body;
    if (req->content_len > 0 && !Http::readBody(req, body, req->content_len)) {
        return Http::sendError(req, 500, "Socket receive failed");
    }
    if (!StorageService::getInstance().writeFile(SETTINGS_FILE, body.c_str())) {
        return Http::sendError(req, 500, "Failed to write config file to SD");
    }
    return Http::sendOk(req, "Config updated successfully");
}

// gemini_config.json: the API key is write-only. GET reports only whether one
// is set; POST keeps the stored key unless the body carries a new one.
esp_err_t getGeminiHandler(httpd_req_t* req) {
    JsonDocument doc;
    std::string content = StorageService::getInstance().readFile(GEMINI_CONFIG);
    if (content.empty() || deserializeJson(doc, content) || !doc.is<JsonObject>()) {
        doc.to<JsonObject>();
        if (!content.empty()) {
            // The firmware then falls back to its compiled-in key (CONFIG_GEMINI_API_KEY).
            doc["config_error"] = "gemini_config.json on the SD card is not valid JSON; it is being ignored";
        }
    }
    const char* key = doc["api_key"] | "";
    doc["api_key_set"] = key[0] != '\0';
    doc.remove("api_key");
    return Http::sendJson(req, 200, doc);
}

esp_err_t setGeminiHandler(httpd_req_t* req) {
    std::string body;
    if (req->content_len == 0 || !Http::readBody(req, body, 8192)) {
        return Http::sendError(req, 400, "Missing or oversized JSON body (max 8 KB)");
    }
    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc.is<JsonObject>()) {
        return Http::sendError(req, 400, "Body must be a JSON object");
    }
    doc.remove("api_key_set");

    const char* new_key = doc["api_key"] | "";
    if (new_key[0] == '\0') {
        JsonDocument current;
        std::string content = StorageService::getInstance().readFile(GEMINI_CONFIG);
        if (!content.empty() && !deserializeJson(current, content) && current["api_key"].is<const char*>()) {
            doc["api_key"] = current["api_key"].as<const char*>();
        } else {
            doc.remove("api_key");
        }
    }

    std::string out;
    serializeJson(doc, out);
    if (!StorageService::getInstance().writeFile(GEMINI_CONFIG, out.c_str())) {
        return Http::sendError(req, 500, "Failed to write config file to SD");
    }
    return Http::sendOk(req, "Gemini config updated; applies to the next session");
}

} // namespace

void Routes::registerConfig(Http::Server& server) {
    server.on("/api/config/settings", HTTP_GET, getSettingsHandler);
    server.on("/api/config/settings", HTTP_POST, setSettingsHandler);
    server.on("/api/config/gemini", HTTP_GET, getGeminiHandler);
    server.on("/api/config/gemini", HTTP_POST, setGeminiHandler);
}
