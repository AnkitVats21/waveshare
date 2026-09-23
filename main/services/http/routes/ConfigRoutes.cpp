#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "services/storage/StorageService.h"
#include "gemini_live/gemini_skills_generated.h"

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

// Pulls "api_key" out of a config file that no longer parses as JSON (e.g.
// truncated), so rewriting the file doesn't lose the key.
std::string salvageApiKey(const std::string& content) {
    size_t k = content.find("\"api_key\"");
    if (k == std::string::npos) return "";
    size_t colon = content.find(':', k);
    size_t open = (colon == std::string::npos) ? colon : content.find('"', colon);
    size_t close = (open == std::string::npos) ? open : content.find('"', open + 1);
    if (close == std::string::npos) return "";
    return content.substr(open + 1, close - open - 1);
}

// Model and voice compiled into the firmware, used when the config sets none.
void addDefaults(JsonDocument& doc) {
    JsonDocument setup;
    if (deserializeJson(setup, GeminiSkills::SETUP_HANDSHAKE_JSON)) return;
    JsonObject defaults = doc["defaults"].to<JsonObject>();
    defaults["model"] = setup["setup"]["model"];
    defaults["voice"] = setup["setup"]["generationConfig"]["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"];
}

// gemini_config.json: {"api_key", "model", "voice", "system_prompt"}. The API
// key is write-only: GET reports only whether one is set; POST keeps the stored
// key unless the body carries a new one.
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
    doc["api_key_set"] = key[0] != '\0' || (doc["config_error"].is<const char*>() && !salvageApiKey(content).empty());
    doc.remove("api_key");
    addDefaults(doc);
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
    doc.remove("defaults");
    doc.remove("config_error");

    const char* new_key = doc["api_key"] | "";
    if (new_key[0] == '\0') {
        JsonDocument current;
        std::string content = StorageService::getInstance().readFile(GEMINI_CONFIG);
        std::string key;
        if (!content.empty() && !deserializeJson(current, content)) {
            key = current["api_key"] | "";
        } else {
            key = salvageApiKey(content);
        }
        if (!key.empty()) {
            doc["api_key"] = key;
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
