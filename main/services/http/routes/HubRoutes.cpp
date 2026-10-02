#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "services/storage/SystemDatabase.h"
#include "credentials/Credentials.h"
#if CONFIG_WAVESHARE_VOICE_HUB
#include "services/hub/HubLink.h"
#endif

#include <ArduinoJson.h>
#include <cstring>

namespace {

// GET /api/hub: the link to the hub. "compiled": false in a build without it.
esp_err_t getHubStatus(httpd_req_t* req) {
    JsonDocument doc;
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
#else
    doc["compiled"] = false;
#endif
    return Http::sendJson(req, 200, doc);
}

// POST /api/hub {"url"?, "token"?}: either may be left out; "" clears it
// (url "" = find the hub by mDNS). The token is never sent back.
esp_err_t setHubConfig(httpd_req_t* req) {
    std::string body;
    JsonDocument doc;
    if (req->content_len == 0 || !Http::readBody(req, body, 1024) || deserializeJson(doc, body)) {
        return Http::sendError(req, 400, "Invalid JSON body");
    }
    if (doc["url"].is<const char*>()) {
        const char* url = doc["url"];
        if (url[0] != '\0' && strncmp(url, "ws://", 5) != 0) {
            return Http::sendError(req, 400, "url must start with ws://");
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
    Services::HubLink::getInstance().reconfigure();
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
