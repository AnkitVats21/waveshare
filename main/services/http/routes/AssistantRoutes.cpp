#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "audio_core/WakeWordEngine.h"
#include "gemini_live/GeminiProtocol.h"
#include "gemini_live/VoiceAgent.h"
#include "gemini_live/TranscriptLog.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "services/http/StateNames.h"
#include "services/storage/SystemDatabase.h"
#include "services/profiles/Profiles.h"
#include "services/BufferManager.h"
#include "app/audio/SpeakerPlayback.h"

#include <cstdlib>

namespace {

esp_err_t manualWake(httpd_req_t* req, uint32_t silence_timeout_ms,
                     const std::string& text = {}) {
    auto& ww = WakeWordEngine::getInstance();
    if (!ww.requestManualWake(silence_timeout_ms)) {
        const char* why = !ww.isRunning()            ? "Wake word engine is not running"
                          : ww.isStreamingActive()   ? "A session is already streaming"
                                                     : "Wake word is suppressed (alarm or recording)";
        return Http::sendError(req, 409, why);
    }
    if (!text.empty()) VoiceAgent::active().sendTextTurn(text);
    return Http::sendOk(req, "Wake requested");
}

// Starts a session as if the wake word was heard. The mic then streams to
// Gemini exactly as after "Alexa", so speech near the device drives the test.
// Keeps the wake word's silence timeout. An optional JSON body
// {"text": "..."} is sent as the person's turn once the session is set up
// (as reminders do), so a test can ask for a reply without speaking.
esp_err_t wakeHandler(httpd_req_t* req) {
    std::string body;
    std::string text;
    if (req->content_len > 0) {
        JsonDocument doc;
        if (!Http::readBody(req, body, 1024) || deserializeJson(doc, body)) {
            return Http::sendError(req, 400, "Expected JSON {\"text\": ...}");
        }
        text = doc["text"] | "";
    }
    return manualWake(req, 0, text);
}

// The dashboard's Start: like wake, but the session waits manual_silence_s
// (a setting) for speech instead of the wake word's 3 s.
esp_err_t startHandler(httpd_req_t* req) {
    return manualWake(req, Services::loadSettings().manual_silence_s * 1000u);
}

// Ends the session in any state: cuts off the reply, closes the connection
// and goes back to waiting for the wake word (via Closing, which chimes).
esp_err_t stopHandler(httpd_req_t* req) {
    auto& sysdb = EmbeddedSysDb::getInstance();
    if (sysdb.snapshot().assistant.session_state == AssistantState::Idle) {
        return Http::sendOk(req, "No session");
    }
    sysdb.mutate([](SystemState& s) {
        if (s.assistant.session_state != AssistantState::Idle) {
            s.assistant.session_state = AssistantState::Closing;
        }
        s.audio.assistant_speaking = false;
        s.audio.turn_complete_pending = false;
    });
    BufferManager::getInstance().flush(Buffers::VOICE_RX_BUF);
    return Http::sendOk(req, "Session ended");
}

// Test hook: moves a live session to a new connection as a goAway would
// (Gemini sends one after ~10 min), at the next turn boundary.
esp_err_t handoffHandler(httpd_req_t* req) {
    if (!GeminiProtocol::getInstance().simulateGoAway()) {
        return Http::sendError(req, 409, "No connected session");
    }
    return Http::sendOk(req, "Handoff requested");
}

esp_err_t statusHandler(httpd_req_t* req) {
    auto snap = EmbeddedSysDb::getInstance().snapshot();
    JsonDocument doc;
    doc["state"] = assistantStateToString(snap.assistant.session_state);
    doc["connection"] = wsStateToString(snap.assistant.ws_state);
    doc["mic_streaming"] = WakeWordEngine::getInstance().isStreamingActive();
    doc["profile"] = snap.assistant.profile;
    return Http::sendJson(req, 200, doc);
}

// Assistant profiles (personas). Voice and model empty = the defaults; see
// Services::Profiles.
//   GET    /api/assistant/profiles           {"active": id, "profiles": [{id, name, voice, model, instructions}]}
//   POST   /api/assistant/profiles           {"id"?, "name", "voice", "model", "instructions"}: without
//          an id creates one; with an id updates it, keeping absent fields. Answers {"id": n}.
//   DELETE /api/assistant/profiles?id=n      the active one hands over to the default (id 1, undeletable)
//   POST   /api/assistant/profiles/activate  {"id": n}: the next session uses it, in a new conversation
esp_err_t listProfilesHandler(httpd_req_t* req) {
    JsonDocument doc;
    doc["active"] = Services::Profiles::activeId();
    doc["default_id"] = Services::DEFAULT_PROFILE_ID;
    doc["max_profiles"] = Services::Profiles::MAX_PROFILES;
    doc["max_instructions"] = Services::Profiles::MAX_INSTRUCTIONS;
    JsonArray list = doc["profiles"].to<JsonArray>();
    for (const auto& [id, p] : Services::listProfiles()) {
        JsonObject o = list.add<JsonObject>();
        o["id"] = id;
        o["name"] = p.name;
        o["voice"] = p.voice;
        o["model"] = p.model;
        o["instructions"] = p.instructions;
    }
    return Http::sendJson(req, 200, doc);
}

esp_err_t saveProfileHandler(httpd_req_t* req) {
    std::string body;
    if (req->content_len == 0 || !Http::readBody(req, body, Services::Profiles::MAX_INSTRUCTIONS + 1024)) {
        return Http::sendError(req, 400, "Missing or oversized JSON body");
    }
    JsonDocument in;
    if (deserializeJson(in, body) || !in.is<JsonObject>()) {
        return Http::sendError(req, 400, "Body must be a JSON object");
    }
    int id = in["id"] | 0;
    ndb::system::ProfileDoc p;
    if (id > 0 && !Services::loadProfile(id, p)) return Http::sendError(req, 404, "No such profile");
    if (id < 0) return Http::sendError(req, 400, "id must be positive");
    for (auto [key, dst] : {std::pair<const char*, std::string*>{"name", &p.name}, {"voice", &p.voice},
                            {"model", &p.model}, {"instructions", &p.instructions}}) {
        if (in[key].is<const char*>()) {
            *dst = in[key].as<const char*>();
        } else if (!in[key].isNull()) {
            return Http::sendError(req, 400, "name, voice, model and instructions must be strings");
        }
    }
    body.clear();
    body.shrink_to_fit();
    std::string error;
    if (!Services::Profiles::save(id, p, error)) return Http::sendError(req, 400, error.c_str());
    JsonDocument out;
    out["id"] = id;
    return Http::sendJson(req, 200, out);
}

esp_err_t deleteProfileHandler(httpd_req_t* req) {
    std::string arg;
    int id = Http::queryParam(req, "id", arg) ? atoi(arg.c_str()) : 0;
    if (id <= 0) return Http::sendError(req, 400, "Missing ?id=");
    std::string error;
    if (!Services::Profiles::remove(id, error)) {
        return Http::sendError(req, error == "no such profile" ? 404 : 400, error.c_str());
    }
    return Http::sendOk(req, "Profile deleted");
}

esp_err_t activateProfileHandler(httpd_req_t* req) {
    std::string body;
    JsonDocument in;
    if (req->content_len == 0 || !Http::readBody(req, body, 256) || deserializeJson(in, body) ||
        !in["id"].is<int>()) {
        return Http::sendError(req, 400, "Body must be {\"id\": n}");
    }
    if (!Services::Profiles::activate(in["id"].as<int>())) return Http::sendError(req, 404, "No such profile");
    return Http::sendOk(req, "Profile active; applies to the next session");
}

// ?since=<seq> returns only entries changed after that seq (poll with the
// "seq" of the previous response).
esp_err_t transcriptHandler(httpd_req_t* req) {
    std::string since;
    uint32_t s = Http::queryParam(req, "since", since) ? strtoul(since.c_str(), nullptr, 10) : 0;
    JsonDocument doc;
    TranscriptLog::instance().toJson(s, doc);
    return Http::sendJson(req, 200, doc);
}

}  // namespace

void Routes::registerAssistant(Http::Server& server) {
    server.on("/api/assistant/wake", HTTP_POST, wakeHandler);
    server.on("/api/assistant/start", HTTP_POST, startHandler);
    server.on("/api/assistant/stop", HTTP_POST, stopHandler);
    server.on("/api/assistant/handoff", HTTP_POST, handoffHandler);
    server.on("/api/assistant/status", HTTP_GET, statusHandler);
    server.on("/api/assistant/transcript", HTTP_GET, transcriptHandler);
    server.on("/api/assistant/profiles", HTTP_GET, listProfilesHandler);
    server.on("/api/assistant/profiles", HTTP_POST, saveProfileHandler);
    server.on("/api/assistant/profiles", HTTP_DELETE, deleteProfileHandler);
    server.on("/api/assistant/profiles/activate", HTTP_POST, activateProfileHandler);
}
