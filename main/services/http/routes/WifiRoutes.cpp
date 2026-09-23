#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "common/sysdb/EmbeddedSysDb.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <cstring>
#include <vector>

namespace {

constexpr const char* TAG = "WifiRoutes";

// OS connectivity probes (Android/iOS/Windows) are redirected to the portal so
// the phone pops up the setup page while on the SoftAP.
esp_err_t captiveRedirectHandler(httpd_req_t* req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t scanHandler(httpd_req_t* req) {
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = true;

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return Http::sendError(req, 500, "Wi-Fi scan failed");
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num > 25) ap_num = 25;

    std::vector<wifi_ap_record_t> ap_records(ap_num);
    if (ap_num > 0) {
        esp_wifi_scan_get_ap_records(&ap_num, ap_records.data());
    }

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (uint16_t i = 0; i < ap_num; ++i) {
        if (ap_records[i].ssid[0] == '\0') continue;
        JsonObject ap = arr.add<JsonObject>();
        ap["ssid"] = reinterpret_cast<char*>(ap_records[i].ssid);
        ap["rssi"] = ap_records[i].rssi;
        ap["auth"] = (ap_records[i].authmode == WIFI_AUTH_OPEN) ? "OPEN" : "SECURED";
    }
    return Http::sendJson(req, 200, doc);
}

esp_err_t configureHandler(httpd_req_t* req) {
    if (req->content_len == 0 || req->content_len >= 256) {
        return Http::sendError(req, 400, "Invalid payload length");
    }
    std::string body;
    if (!Http::readBody(req, body, 255)) {
        return Http::sendError(req, 500, "Failed to read request body");
    }

    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc["ssid"].is<const char*>()) {
        return Http::sendError(req, 400, "JSON must contain 'ssid'");
    }

    std::string ssid = doc["ssid"].as<std::string>();
    std::string pass = doc["password"] | "";
    if (ssid.empty()) {
        return Http::sendError(req, 400, "SSID cannot be empty");
    }

    ESP_LOGI(TAG, "Wi-Fi credentials received for SSID '%s', initiating connection...", ssid.c_str());

    EmbeddedSysDb::getInstance().mutate([&ssid, &pass](SystemState& s) {
        std::strncpy(s.system.wifi_ssid, ssid.c_str(), sizeof(s.system.wifi_ssid) - 1);
        s.system.wifi_ssid[sizeof(s.system.wifi_ssid) - 1] = '\0';
        std::strncpy(s.system.wifi_password, pass.c_str(), sizeof(s.system.wifi_password) - 1);
        s.system.wifi_password[sizeof(s.system.wifi_password) - 1] = '\0';
        s.system.network_state = NetworkState::Connecting;
        s.system.wifi_apply_creds = true;
    });

    return Http::sendOk(req, "Credentials received. Connecting to Wi-Fi...");
}

esp_err_t statusHandler(httpd_req_t* req) {
    SystemState snap = EmbeddedSysDb::getInstance().snapshot();

    const char* state_str = "Disconnected";
    switch (snap.system.network_state) {
        case NetworkState::Connecting:   state_str = "Connecting"; break;
        case NetworkState::Connected:    state_str = "Connected"; break;
        case NetworkState::PortalActive: state_str = "PortalActive"; break;
        case NetworkState::Failed:       state_str = "Failed"; break;
        default: break;
    }

    JsonDocument doc;
    doc["network_state"] = state_str;
    doc["wifi_connected"] = snap.system.wifi_connected;
    doc["ap_active"] = snap.system.ap_active;
    doc["ssid"] = snap.system.wifi_ssid;
    return Http::sendJson(req, 200, doc);
}

} // namespace

void Routes::registerWifi(Http::Server& server) {
    for (const char* probe : { "/generate_204", "/gen_204", "/hotspot-detect.html",
                               "/connecttest.txt", "/ncsi.txt", "/canonical.html" }) {
        server.on(probe, HTTP_GET, captiveRedirectHandler);
    }
    server.on("/api/wifi/scan", HTTP_GET, scanHandler);
    server.on("/api/wifi/configure", HTTP_POST, configureHandler);
    server.on("/api/wifi/status", HTTP_GET, statusHandler);
}
