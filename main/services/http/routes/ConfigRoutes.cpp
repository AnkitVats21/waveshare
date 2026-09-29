#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "services/storage/SystemDatabase.h"
#include "services/time/TimeSyncHelper.h"
#include "credentials/Credentials.h"
#include "app/audio/AudioService.h"
#include "gemini_live/gemini_skills_generated.h"
#include "gemini_live/GeminiProtocol.h"
#include "media_player/TlsConfig.h"

#include <esp_http_client.h>

namespace {

using ndb::system::Settings;

constexpr int MIN_SILENCE_S = 3;
constexpr int MAX_SILENCE_S = 60;
constexpr int MAX_RESUME_MIN = 120;   // Gemini keeps a resumption handle ~2 h
constexpr int MAX_KEEPALIVE_S = 180;

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
//   keepalive_s:      the connection stays open this long after a session,
//                     so a quick follow-up wake skips connecting (0 = close)
//   echo_measure:     log how much echo the mic picks up during replies
//   barge_in:         keep the mic streaming during replies, so talking
//                     over one interrupts it
//   web_search:       give Gemini Google Search on the models that have it
//                     (2.5 Live; not 3.x); the others use the MCP search
//   weather_location: home city for the weather tool, e.g. "Pune"
//   vad_start, vad_end: Gemini's start/end-of-speech sensitivity,
//                     0 = its default, 1 = low, 2 = high
//   vad_prefix_ms, vad_silence_ms: speech needed before a start counts, and
//                     silence that ends a turn (0 = Gemini's default)
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
    doc["keepalive_s"] = s.keepalive_s;
    doc["echo_measure"] = s.echo_measure;
    doc["barge_in"] = s.barge_in;
    doc["web_search"] = s.web_search;
    doc["weather_location"] = s.weather_location;
    doc["vad_start"] = s.vad_start;
    doc["vad_end"] = s.vad_end;
    doc["vad_prefix_ms"] = s.vad_prefix_ms;
    doc["vad_silence_ms"] = s.vad_silence_ms;
    doc["api_key_set"] = credentials::hasGeminiApiKey();
    addDefaults(doc);
    return Http::sendJson(req, 200, doc);
}

esp_err_t appendBody(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        static_cast<std::string*>(evt->user_data)->append(static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

// The models this key can run a voice session on, asked from Google (the
// key never leaves the device): those supporting bidiGenerateContent, less
// the transcribe, translate and robotics ones. "search" says whether Google
// Search works there: "no" if the model refused it this boot, "yes" for the
// 2.5 native-audio models (tested; Gemini 3.x on the free tier refuses it),
// else "unknown". Blocks the server for up to ~8 s; the list is ~30 KB.
esp_err_t getGeminiModelsHandler(httpd_req_t* req) {
    std::string key = credentials::geminiApiKey();
    if (key.empty()) return Http::sendError(req, 409, "No Gemini API key is set");

    std::string body;
    esp_http_client_config_t config = {};
    config.url = "https://generativelanguage.googleapis.com/v1beta/models?pageSize=1000";
    config.event_handler = appendBody;
    config.user_data = &body;
    config.timeout_ms = 8000;
    config.buffer_size = 4096;
    Tls::secure(config);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return Http::sendError(req, 503, "Out of memory for the request");
    esp_http_client_set_header(client, "x-goog-api-key", key.c_str());
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW("ConfigRoutes", "Model list failed: %s, HTTP %d", esp_err_to_name(err), status);
        return Http::sendError(req, 502, "Google did not return the model list");
    }

    JsonDocument filter;
    filter["models"][0]["name"] = true;
    filter["models"][0]["displayName"] = true;
    filter["models"][0]["supportedGenerationMethods"] = true;
    JsonDocument google;
    if (deserializeJson(google, body, DeserializationOption::Filter(filter))) {
        return Http::sendError(req, 502, "Could not parse the model list");
    }
    body.clear();
    body.shrink_to_fit();

    std::string refused = GeminiProtocol::getInstance().searchRefusedModel();
    JsonDocument doc;
    JsonArray out = doc["models"].to<JsonArray>();
    for (JsonObject m : google["models"].as<JsonArray>()) {
        bool live = false;
        for (const char* method : m["supportedGenerationMethods"].as<JsonArray>()) {
            if (method && strcmp(method, "bidiGenerateContent") == 0) live = true;
        }
        std::string name = m["name"] | "";
        if (!live || name.find("transcribe") != std::string::npos ||
            name.find("translate") != std::string::npos || name.find("robotics") != std::string::npos) {
            continue;
        }
        JsonObject o = out.add<JsonObject>();
        o["name"] = name.rfind("models/", 0) == 0 ? name.substr(7) : name;
        o["display_name"] = m["displayName"] | "";
        o["search"] = name == refused                                           ? "no"
                      : name.rfind("models/gemini-2.5-flash-native-audio", 0) == 0 ? "yes"
                                                                                 : "unknown";
    }
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
    take("weather_location", s.weather_location, Settings::F_WEATHER_LOCATION);
    if (s.weather_location.size() > 64) return Http::sendError(req, 400, "weather_location is too long");
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
    if (doc["keepalive_s"].is<int>()) {
        int v = doc["keepalive_s"];
        if (v < 0 || v > MAX_KEEPALIVE_S) {
            return Http::sendError(req, 400, "keepalive_s must be 0-180");
        }
        s.keepalive_s = v;
        fields |= Settings::F_KEEPALIVE_S;
    }
    if (doc["echo_measure"].is<bool>()) {
        s.echo_measure = doc["echo_measure"];
        fields |= Settings::F_ECHO_MEASURE;
    }
    if (doc["barge_in"].is<bool>()) {
        s.barge_in = doc["barge_in"];
        fields |= Settings::F_BARGE_IN;
    }
    auto takeInt = [&](const char* key, int max, uint64_t bit, auto& dst) {
        if (!doc[key].is<int>()) return true;
        int v = doc[key];
        if (v < 0 || v > max) return false;
        dst = v;
        fields |= bit;
        return true;
    };
    if (!takeInt("vad_start", 2, Settings::F_VAD_START, s.vad_start) ||
        !takeInt("vad_end", 2, Settings::F_VAD_END, s.vad_end)) {
        return Http::sendError(req, 400, "vad_start and vad_end must be 0-2");
    }
    if (!takeInt("vad_prefix_ms", 2000, Settings::F_VAD_PREFIX_MS, s.vad_prefix_ms) ||
        !takeInt("vad_silence_ms", 5000, Settings::F_VAD_SILENCE_MS, s.vad_silence_ms)) {
        return Http::sendError(req, 400, "vad_prefix_ms must be 0-2000, vad_silence_ms 0-5000");
    }
    if (doc["web_search"].is<bool>()) {
        s.web_search = doc["web_search"];
        fields |= Settings::F_WEB_SEARCH;
    }
    // Stored after validation, so a rejected request changes nothing.
    std::string new_key = doc["api_key"] | "";
    if (!new_key.empty() && !credentials::setGeminiApiKey(new_key)) {
        return Http::sendError(req, 500, "Failed to store the API key");
    }

    if (fields && !Services::saveSettings(s, fields)) {
        return Http::sendError(req, 500, "Failed to save settings");
    }
    if (fields & (Settings::F_ECHO_MEASURE | Settings::F_BARGE_IN)) {
        Settings now = Services::loadSettings();
        AudioService::setVoiceOptions(now.echo_measure, now.barge_in);
    }
    return Http::sendOk(req, "Gemini config updated; applies to the next session");
}

} // namespace

void Routes::registerConfig(Http::Server& server) {
    server.on("/api/config/settings", HTTP_GET, getSettingsHandler);
    server.on("/api/config/settings", HTTP_POST, setSettingsHandler);
    server.on("/api/config/gemini", HTTP_GET, getGeminiHandler);
    server.on("/api/config/gemini", HTTP_POST, setGeminiHandler);
    server.on("/api/config/gemini/models", HTTP_GET, getGeminiModelsHandler);
}
