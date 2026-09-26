#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "services/storage/SystemDatabase.h"
#include "services/time/TimeSyncHelper.h"
#include "credentials/Credentials.h"
#include "gemini_live/gemini_skills_generated.h"

namespace {

using ndb::system::Settings;

// {"timezone": "IST-5:30"}: a POSIX TZ string, applied immediately.
esp_err_t getSettingsHandler(httpd_req_t* req) {
    JsonDocument doc;
    doc["timezone"] = Services::loadSettings().timezone;
    return Http::sendJson(req, 200, doc);
}

esp_err_t setSettingsHandler(httpd_req_t* req) {
    std::string body;
    JsonDocument doc;
    if (req->content_len == 0 || !Http::readBody(req, body, 512) || deserializeJson(doc, body) ||
        !doc["timezone"].is<const char*>()) {
        return Http::sendError(req, 400, "Body must be {\"timezone\": \"<POSIX TZ>\"}");
    }
    Settings s;
    s.timezone = doc["timezone"].as<const char*>();
    if (s.timezone.empty()) s.timezone = "UTC";
    if (!Services::saveSettings(s, Settings::F_TIMEZONE)) {
        return Http::sendError(req, 500, "Failed to save settings");
    }
    Services::TimeSyncHelper::applyTimezone(s.timezone);
    return Http::sendOk(req, "Settings updated");
}

// Model and voice compiled into the firmware, used when the config sets none.
void addDefaults(JsonDocument& doc) {
    JsonDocument setup;
    if (deserializeJson(setup, GeminiSkills::SETUP_HANDSHAKE_JSON)) return;
    JsonObject defaults = doc["defaults"].to<JsonObject>();
    defaults["model"] = setup["setup"]["model"];
    defaults["voice"] = setup["setup"]["generationConfig"]["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"];
}

// Model, voice and system prompt live in system.ndb; the API key in NVS. The
// key is write-only: GET reports only whether one is set; POST stores a new
// key when the body carries one. Unset fields are omitted, and the firmware
// defaults are listed under "defaults".
esp_err_t getGeminiHandler(httpd_req_t* req) {
    Settings s = Services::loadSettings();
    JsonDocument doc;
    doc.to<JsonObject>();
    if (!s.gemini_model.empty()) doc["model"] = s.gemini_model;
    if (!s.gemini_voice.empty()) doc["voice"] = s.gemini_voice;
    if (!s.gemini_system_prompt.empty()) doc["system_prompt"] = s.gemini_system_prompt;
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

    std::string new_key = doc["api_key"] | "";
    if (!new_key.empty() && !credentials::setGeminiApiKey(new_key)) {
        return Http::sendError(req, 500, "Failed to store the API key");
    }

    // A field present in the body is set (an empty string clears it); an
    // absent one is left unchanged.
    Settings s;
    uint64_t fields = 0;
    auto take = [&](const char* key, std::string& dst, uint64_t bit) {
        if (doc[key].is<const char*>()) {
            dst = doc[key].as<const char*>();
            fields |= bit;
        }
    };
    take("model", s.gemini_model, Settings::F_GEMINI_MODEL);
    take("voice", s.gemini_voice, Settings::F_GEMINI_VOICE);
    take("system_prompt", s.gemini_system_prompt, Settings::F_GEMINI_SYSTEM_PROMPT);
    if (fields && !Services::saveSettings(s, fields)) {
        return Http::sendError(req, 500, "Failed to save settings");
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
