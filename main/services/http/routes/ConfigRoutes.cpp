#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "sd_storage/Fs.h"
#include "credentials/Credentials.h"
#include "gemini_live/gemini_skills_generated.h"

namespace {

constexpr const char* GEMINI_CONFIG = "/sdcard/gemini_config.json";
constexpr const char* SETTINGS_FILE = "/sdcard/settings.txt";

// settings.txt: raw text passthrough.
esp_err_t getSettingsHandler(httpd_req_t* req) {
    std::string content = sd_storage::Fs::readText(SETTINGS_FILE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, content.c_str(), content.length());
}

esp_err_t setSettingsHandler(httpd_req_t* req) {
    std::string body;
    if (req->content_len > 0 && !Http::readBody(req, body, req->content_len)) {
        return Http::sendError(req, 500, "Socket receive failed");
    }
    if (!sd_storage::Fs::writeAtomic(SETTINGS_FILE, body.c_str())) {
        return Http::sendError(req, 500, "Failed to write config file to SD");
    }
    return Http::sendOk(req, "Config updated successfully");
}

// Model and voice compiled into the firmware, used when the config sets none.
void addDefaults(JsonDocument& doc) {
    JsonDocument setup;
    if (deserializeJson(setup, GeminiSkills::SETUP_HANDSHAKE_JSON)) return;
    JsonObject defaults = doc["defaults"].to<JsonObject>();
    defaults["model"] = setup["setup"]["model"];
    defaults["voice"] = setup["setup"]["generationConfig"]["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"];
}

// gemini_config.json holds {"model", "voice", "system_prompt"}; the API key is
// in NVS. The key is write-only: GET reports only whether one is set; POST
// stores a new key when the body carries one and never writes it to the file.
esp_err_t getGeminiHandler(httpd_req_t* req) {
    JsonDocument doc;
    std::string content = sd_storage::Fs::readText(GEMINI_CONFIG);
    if (content.empty() || deserializeJson(doc, content) || !doc.is<JsonObject>()) {
        doc.to<JsonObject>();
        if (!content.empty()) {
            doc["config_error"] = "gemini_config.json on the SD card is not valid JSON; it is being ignored";
        }
    }
    doc.remove("api_key");
    doc["api_key_set"] = credentials::hasGeminiApiKey();
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

    std::string new_key = doc["api_key"] | "";
    doc.remove("api_key");
    if (!new_key.empty() && !credentials::setGeminiApiKey(new_key)) {
        return Http::sendError(req, 500, "Failed to store the API key");
    }

    std::string out;
    serializeJson(doc, out);
    if (!sd_storage::Fs::writeAtomic(GEMINI_CONFIG, out.c_str())) {
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
