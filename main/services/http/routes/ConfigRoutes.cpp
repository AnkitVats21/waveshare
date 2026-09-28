#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "services/storage/SystemDatabase.h"
#include "services/time/TimeSyncHelper.h"
#include "credentials/Credentials.h"
#include "gemini_live/gemini_skills_generated.h"

namespace {

using ndb::system::Settings;

constexpr int MIN_SILENCE_S = 3;
constexpr int MAX_SILENCE_S = 60;
constexpr int MAX_RESUME_MIN = 120;   // Gemini keeps a resumption handle ~2 h

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

// Model, voice, system prompt and the transcript and session options live in
// system.ndb; the API key in NVS. The key is write-only: GET reports only
// whether one is set; POST stores a new key when the body carries one. Unset
// strings are omitted, and the firmware defaults are listed under "defaults".
//   transcripts:      ask Gemini for transcriptions (shown on the dashboard)
//   transcript_log:   also print each turn to the log
//   manual_silence_s: silence timeout of a session started from the dashboard
//   resume_min:       a session within this many minutes of the last one
//                     continues that conversation (0 = always start fresh)
esp_err_t getGeminiHandler(httpd_req_t* req) {
    Settings s = Services::loadSettings();
    JsonDocument doc;
    doc.to<JsonObject>();
    if (!s.gemini_model.empty()) doc["model"] = s.gemini_model;
    if (!s.gemini_voice.empty()) doc["voice"] = s.gemini_voice;
    if (!s.gemini_system_prompt.empty()) doc["system_prompt"] = s.gemini_system_prompt;
    doc["transcripts"] = s.transcripts;
    doc["transcript_log"] = s.transcript_log;
    doc["manual_silence_s"] = s.manual_silence_s;
    doc["resume_min"] = s.resume_min;
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
    if (doc["transcripts"].is<bool>()) {
        s.transcripts = doc["transcripts"];
        fields |= Settings::F_TRANSCRIPTS;
    }
    if (doc["transcript_log"].is<bool>()) {
        s.transcript_log = doc["transcript_log"];
        fields |= Settings::F_TRANSCRIPT_LOG;
    }
    if (doc["manual_silence_s"].is<int>()) {
        int v = doc["manual_silence_s"];
        if (v < MIN_SILENCE_S || v > MAX_SILENCE_S) {
            return Http::sendError(req, 400, "manual_silence_s must be 3-60");
        }
        s.manual_silence_s = v;
        fields |= Settings::F_MANUAL_SILENCE_S;
    }
    if (doc["resume_min"].is<int>()) {
        int v = doc["resume_min"];
        if (v < 0 || v > MAX_RESUME_MIN) {
            return Http::sendError(req, 400, "resume_min must be 0-120");
        }
        s.resume_min = v;
        fields |= Settings::F_RESUME_MIN;
    }
    // Stored after validation, so a rejected request changes nothing.
    std::string new_key = doc["api_key"] | "";
    if (!new_key.empty() && !credentials::setGeminiApiKey(new_key)) {
        return Http::sendError(req, 500, "Failed to store the API key");
    }

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
