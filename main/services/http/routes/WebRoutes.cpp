#include "services/http/routes/Routes.h"
#include "services/http/web/WebDashboardHtml.h"
#include "services/http/web/CaptivePortalHtml.h"
#include "common/sysdb/EmbeddedSysDb.h"

#include <cstring>

namespace {

// The captive portal while the SoftAP is up (or on /setup), else the dashboard.
esp_err_t indexHandler(httpd_req_t* req) {
    SystemState snap = EmbeddedSysDb::getInstance().snapshot();
    httpd_resp_set_type(req, "text/html");
    if (snap.system.ap_active || std::strstr(req->uri, "/setup") != nullptr) {
        return httpd_resp_send(req, Services::CAPTIVE_PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send(req, Services::WEB_DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
}

} // namespace

void Routes::registerWeb(Http::Server& server) {
    server.on("/", HTTP_GET, indexHandler);
    server.on("/index.html", HTTP_GET, indexHandler);
    server.on("/setup", HTTP_GET, indexHandler);
}
