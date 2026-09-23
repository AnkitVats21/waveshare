#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "services/storage/StorageService.h"

#include <cstring>

using Services::StorageService;

namespace {

constexpr const char* GEMINI_CONFIG = "/sdcard/gemini_config.json";
constexpr const char* SETTINGS_FILE = "/sdcard/settings.txt";

bool isGemini(httpd_req_t* req) {
    return std::strstr(req->uri, "gemini") != nullptr;
}

// Raw passthrough of the config file on the SD card.
esp_err_t getHandler(httpd_req_t* req) {
    bool gemini = isGemini(req);
    std::string content = StorageService::getInstance().readFile(gemini ? GEMINI_CONFIG : SETTINGS_FILE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, gemini ? "application/json" : "text/plain");
    return httpd_resp_send(req, content.c_str(), content.length());
}

esp_err_t setHandler(httpd_req_t* req) {
    std::string body;
    if (req->content_len > 0 && !Http::readBody(req, body, req->content_len)) {
        return Http::sendError(req, 500, "Socket receive failed");
    }
    if (!StorageService::getInstance().writeFile(isGemini(req) ? GEMINI_CONFIG : SETTINGS_FILE, body.c_str())) {
        return Http::sendError(req, 500, "Failed to write config file to SD");
    }
    return Http::sendOk(req, "Config updated successfully");
}

} // namespace

void Routes::registerConfig(Http::Server& server) {
    server.on("/api/config/settings", HTTP_GET, getHandler);
    server.on("/api/config/settings", HTTP_POST, setHandler);
    server.on("/api/config/gemini", HTTP_GET, getHandler);
    server.on("/api/config/gemini", HTTP_POST, setHandler);
}
