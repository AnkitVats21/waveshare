#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "app/audio/recording/AudioRecorder.h"
#include "audio_core/AlertPlayer.h"

#include <cstdlib>

namespace {

constexpr size_t MAX_BODY = 1024;

// Parses a small JSON body into doc; false if absent or invalid.
bool readJsonBody(httpd_req_t* req, JsonDocument& doc) {
    std::string body;
    return Http::readBody(req, body, MAX_BODY) && !deserializeJson(doc, body);
}

esp_err_t volumeHandler(httpd_req_t* req) {
    std::string val;
    int vol = -1;
    JsonDocument body;
    if (Http::queryParam(req, "value", val)) {
        vol = atoi(val.c_str());
    } else if (readJsonBody(req, body)) {
        vol = body["volume"] | body["value"] | -1;
    }
    if (vol < 0 || vol > 100) return Http::sendError(req, 400, "Volume must be between 0 and 100");

    EmbeddedSysDb::getInstance().mutate([vol](SystemState& s) { s.audio.speaker_volume = vol; });

    JsonDocument doc;
    doc["status"] = "ok";
    doc["speaker_volume"] = vol;
    return Http::sendJson(req, 200, doc);
}

esp_err_t micGainHandler(httpd_req_t* req) {
    std::string val;
    float gain = -1.0f;
    JsonDocument body;
    if (Http::queryParam(req, "value", val)) {
        gain = static_cast<float>(atof(val.c_str()));
    } else if (readJsonBody(req, body)) {
        gain = body["gain"] | body["value"] | -1.0f;
    }
    if (gain < 0.0f || gain > 60.0f) return Http::sendError(req, 400, "Mic gain must be between 0 and 60.0 dB");

    EmbeddedSysDb::getInstance().mutate([gain](SystemState& s) { s.audio.mic_gain_db = gain; });

    JsonDocument doc;
    doc["status"] = "ok";
    doc["mic_gain_db"] = gain;
    return Http::sendJson(req, 200, doc);
}

esp_err_t micMuteHandler(httpd_req_t* req) {
    std::string val;
    bool muted = false;
    JsonDocument body;
    if (Http::queryParam(req, "muted", val)) {
        muted = (val == "1" || val == "true");
    } else if (readJsonBody(req, body)) {
        muted = body["muted"] | false;
    }

    EmbeddedSysDb::getInstance().mutate([muted](SystemState& s) { s.audio.mic_enabled = !muted; });

    JsonDocument doc;
    doc["status"] = "ok";
    doc["mic_enabled"] = !muted;
    return Http::sendJson(req, 200, doc);
}

esp_err_t alertHandler(httpd_req_t* req) {
    AlertPlayer::getInstance().playAlert(ALERT_WAKE_CONFIRM);
    return Http::sendOk(req, "Alert chime triggered");
}

esp_err_t recordHandler(httpd_req_t* req) {
    auto& recorder = AudioRecorder::getInstance();
    bool is_start = std::string(req->uri).find("/start") != std::string::npos;
    if (is_start) {
        if (!recorder.startRecording(AudioRecorder::RecordMode::RAW)) {
            return Http::sendError(req, 409, "Recording is already active");
        }
    } else {
        recorder.stopRecording(AudioRecorder::StopReason::MANUAL);
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["is_recording"] = recorder.isRecording();
    return Http::sendJson(req, 200, doc);
}

} // namespace

void Routes::registerAudio(Http::Server& server) {
    server.on("/api/audio/volume", HTTP_POST, volumeHandler);
    server.on("/api/audio/mic_gain", HTTP_POST, micGainHandler);
    server.on("/api/audio/mic_mute", HTTP_POST, micMuteHandler);
    server.on("/api/audio/alert", HTTP_POST, alertHandler);
    server.on("/api/audio/record/start", HTTP_POST, recordHandler);
    server.on("/api/audio/record/stop", HTTP_POST, recordHandler);
}
