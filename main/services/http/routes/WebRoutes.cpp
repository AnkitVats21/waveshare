#include "services/http/routes/Routes.h"
#include "services/http/web/WebDashboardHtml.h"
#include "services/http/web/CaptivePortalHtml.h"
#include "http_server/HttpUtil.h"
#include "http_server/WebBundle.h"
#include "common/sysdb/EmbeddedSysDb.h"

#include <cstring>

namespace {

esp_err_t sendPortal(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, Services::CAPTIVE_PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

// Built-in dashboard compiled into the firmware: always reachable at
// /recovery, and served at / when no frontend bundle is installed.
esp_err_t sendRecovery(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, Services::WEB_DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
}

bool isAppShell(const char* uri) {
    size_t len = strcspn(uri, "?#");
    return (len == 1 && uri[0] == '/') || (len == 11 && strncmp(uri, "/index.html", 11) == 0);
}

// Catch-all GET (registered last): the frontend bundle, with the captive
// portal taking over while the setup SoftAP is up.
esp_err_t frontendHandler(httpd_req_t* req) {
    if (strncmp(req->uri, "/api/", 5) == 0) {
        return Http::sendError(req, 404, "Unknown API endpoint");
    }
    if (EmbeddedSysDb::getInstance().snapshot().system.ap_active) {
        return sendPortal(req);
    }
    esp_err_t err = Http::WebBundle::instance().serve(req);
    if (err != ESP_ERR_NOT_FOUND) return err;
    if (isAppShell(req->uri)) return sendRecovery(req);
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

} // namespace

void Routes::registerWeb(Http::Server& server) {
    server.on("/setup", HTTP_GET, [](httpd_req_t* req) { return sendPortal(req); });
    server.on("/recovery", HTTP_GET, sendRecovery);
}

void Routes::registerFrontend(Http::Server& server) {
    server.on("/*", HTTP_GET, frontendHandler);
}
