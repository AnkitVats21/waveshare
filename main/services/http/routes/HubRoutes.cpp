#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "services/storage/SystemDatabase.h"
#include "credentials/Credentials.h"
#if CONFIG_WAVESHARE_VOICE_HUB
#include "services/hub/HubAgent.h"
#include "services/hub/HubLink.h"
#endif

#include <ArduinoJson.h>
#include <cstring>

namespace {

const char* const BACKENDS[] = {"direct", "hub", "auto"};   // voice_backend values
const char* const MUSIC[] = {"auto", "local"};               // music_control values
const char* const CODECS[] = {"auto", "pcm", "opus"};        // uplink_codec values

// GET /api/hub: the link to the hub. "compiled": false in a build without it.
esp_err_t getHubStatus(httpd_req_t* req) {
    JsonDocument doc;
    auto settings = Services::loadSettings();
    uint8_t backend = settings.voice_backend;
    doc["backend"] = backend < 3 ? BACKENDS[backend] : "direct";
    uint8_t music = settings.music_control;
    doc["music"] = music < 2 ? MUSIC[music] : "auto";
    uint8_t codec = settings.uplink_codec;
    doc["uplink_codec"] = codec < 3 ? CODECS[codec] : "auto";
    doc["uplink_opus_kbps"] = settings.uplink_opus_kbps;
    doc["uplink_opus_complexity"] = settings.uplink_opus_complexity;
#if CONFIG_WAVESHARE_VOICE_HUB
    auto s = Services::HubLink::getInstance().status();
    doc["compiled"] = true;
    doc["up"] = s.up;
    doc["connected"] = s.connected;
    doc["configured_url"] = Services::loadSettings().hub_url;
    doc["url"] = s.url;
    doc["from_mdns"] = s.from_mdns;
    doc["hub_version"] = s.hub_version;
    doc["last_error"] = s.last_error;
    doc["has_token"] = !credentials::hubToken().empty();
    doc["music_active"] = Services::HubLink::getInstance().musicAvailable();
#else
    doc["compiled"] = false;
#endif
    return Http::sendJson(req, 200, doc);
}

// POST /api/hub {"url"?, "token"?, "backend"?, "music"?}: any may be left
// out; "" clears url or token (url "" = find the hub by mDNS; url must start
// with ws:// or wss://). The token is never sent back. backend ("direct",
// "hub", "auto") applies from the next session; music ("auto": the hub runs it
// when up, "local") reconnects.
esp_err_t setHubConfig(httpd_req_t* req) {
    std::string body;
    JsonDocument doc;
    if (req->content_len == 0 || !Http::readBody(req, body, 1024) || deserializeJson(doc, body)) {
        return Http::sendError(req, 400, "Invalid JSON body");
    }
    if (doc["backend"].is<const char*>()) {
        const char* name = doc["backend"];
        int backend = -1;
        for (int i = 0; i < 3; ++i) {
            if (strcmp(name, BACKENDS[i]) == 0) backend = i;
        }
        if (backend < 0) return Http::sendError(req, 400, "backend must be direct, hub or auto");
        auto settings = Services::loadSettings();
        settings.voice_backend = (uint8_t)backend;
        if (!Services::saveSettings(settings, ndb::system::Settings::F_VOICE_BACKEND)) {
            return Http::sendError(req, 500, "Could not save the backend");
        }
#if CONFIG_WAVESHARE_VOICE_HUB
        Services::HubAgent::setBackendSetting((uint8_t)backend);
#endif
    }
    bool link_changed = doc["url"].is<const char*>() || doc["token"].is<const char*>();
    if (doc["music"].is<const char*>()) {
        const char* name = doc["music"];
        int music = -1;
        for (int i = 0; i < 2; ++i) {
            if (strcmp(name, MUSIC[i]) == 0) music = i;
        }
        if (music < 0) return Http::sendError(req, 400, "music must be auto or local");
        auto settings = Services::loadSettings();
        settings.music_control = (uint8_t)music;
        if (!Services::saveSettings(settings, ndb::system::Settings::F_MUSIC_CONTROL)) {
            return Http::sendError(req, 500, "Could not save the music setting");
        }
#if CONFIG_WAVESHARE_VOICE_HUB
        Services::HubLink::setMusicControl((uint8_t)music);
#endif
        link_changed = true;   // the hello says the roles again
    }
    if (doc["uplink_codec"].is<const char*>()) {
        const char* name = doc["uplink_codec"];
        int codec = -1;
        for (int i = 0; i < 3; ++i) {
            if (strcmp(name, CODECS[i]) == 0) codec = i;
        }
        if (codec < 0) return Http::sendError(req, 400, "uplink_codec must be auto, pcm or opus");
        auto settings = Services::loadSettings();
        settings.uplink_codec = (uint8_t)codec;
        if (!Services::saveSettings(settings, ndb::system::Settings::F_UPLINK_CODEC)) {
            return Http::sendError(req, 500, "Could not save the uplink codec");
        }
#if CONFIG_WAVESHARE_VOICE_HUB
        Services::HubLink::setUplinkCodec((uint8_t)codec);
#endif
        link_changed = true;   // the hello says the offered formats again
    }
    if (doc["uplink_opus_kbps"].is<int>()) {
        int kbps = doc["uplink_opus_kbps"].as<int>();
        if (kbps < 12) kbps = 12;
        if (kbps > 64) kbps = 64;
        auto settings = Services::loadSettings();
        settings.uplink_opus_kbps = (uint8_t)kbps;
        if (!Services::saveSettings(settings, ndb::system::Settings::F_UPLINK_OPUS_KBPS)) {
            return Http::sendError(req, 500, "Could not save the opus bitrate");
        }
    }
    if (doc["uplink_opus_complexity"].is<int>()) {
        int complexity = doc["uplink_opus_complexity"].as<int>();
        if (complexity < 0) complexity = 0;
        if (complexity > 10) complexity = 10;
        auto settings = Services::loadSettings();
        settings.uplink_opus_complexity = (uint8_t)complexity;
        if (!Services::saveSettings(settings, ndb::system::Settings::F_UPLINK_OPUS_COMPLEXITY)) {
            return Http::sendError(req, 500, "Could not save the opus complexity");
        }
    }
    if (doc["url"].is<const char*>()) {
        const char* url = doc["url"];
        if (url[0] != '\0' && strncmp(url, "ws://", 5) != 0 && strncmp(url, "wss://", 6) != 0) {
            return Http::sendError(req, 400, "url must start with ws:// or wss://");
        }
        auto settings = Services::loadSettings();
        settings.hub_url = url;
        if (!Services::saveSettings(settings, ndb::system::Settings::F_HUB_URL)) {
            return Http::sendError(req, 500, "Could not save the URL");
        }
    }
    if (doc["token"].is<const char*>() && !credentials::setHubToken(doc["token"].as<const char*>())) {
        return Http::sendError(req, 500, "Could not save the token");
    }
#if CONFIG_WAVESHARE_VOICE_HUB
    if (link_changed) Services::HubLink::getInstance().reconfigure();
#else
    (void)link_changed;
#endif
    return Http::sendOk(req, "Hub configuration updated");
}

} // namespace

namespace Routes {

void registerHub(Http::Server& server) {
    server.on("/api/hub", HTTP_GET, getHubStatus);
    server.on("/api/hub", HTTP_POST, setHubConfig);
}

} // namespace Routes
