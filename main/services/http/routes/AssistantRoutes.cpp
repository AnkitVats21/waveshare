#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "audio_core/WakeWordEngine.h"
#include "gemini_live/TranscriptLog.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "services/http/StateNames.h"

#include <cstdlib>

namespace {

// Starts a session as if the wake word was heard. The mic then streams to
// Gemini exactly as after "Alexa", so speech near the device drives the test.
esp_err_t wakeHandler(httpd_req_t* req) {
    auto& ww = WakeWordEngine::getInstance();
    if (!ww.requestManualWake()) {
        const char* why = !ww.isRunning()            ? "Wake word engine is not running"
                          : ww.isStreamingActive()   ? "A session is already streaming"
                                                     : "Wake word is suppressed (alarm or recording)";
        return Http::sendError(req, 409, why);
    }
    return Http::sendOk(req, "Wake requested");
}

esp_err_t statusHandler(httpd_req_t* req) {
    auto snap = EmbeddedSysDb::getInstance().snapshot();
    JsonDocument doc;
    doc["state"] = assistantStateToString(snap.assistant.session_state);
    doc["connection"] = wsStateToString(snap.assistant.ws_state);
    doc["mic_streaming"] = WakeWordEngine::getInstance().isStreamingActive();
    return Http::sendJson(req, 200, doc);
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
    server.on("/api/assistant/status", HTTP_GET, statusHandler);
    server.on("/api/assistant/transcript", HTTP_GET, transcriptHandler);
}
