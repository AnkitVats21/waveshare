// /api/time: the device clock.
//
//   GET  /api/time        clock, timezone, NTP state
//   POST /api/time        {"epoch": <UTC seconds>, "timezone"?: "<POSIX TZ>"} set by hand
//   POST /api/time/sync   NTP sync now (runs in the background; poll GET)
#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "services/storage/SystemDatabase.h"
#include "services/time/TimeSyncHelper.h"

#include <cmath>
#include <ctime>

namespace {

using Services::TimeSyncHelper;

const char* sourceName(TimeSyncHelper::Source s) {
    return s == TimeSyncHelper::Source::Ntp ? "ntp" : s == TimeSyncHelper::Source::Manual ? "manual" : "none";
}

esp_err_t sendTime(httpd_req_t* req, const char* message = nullptr) {
    TimeSyncHelper::Status st = TimeSyncHelper::instance().status();
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char local[32];
    strftime(local, sizeof(local), "%Y-%m-%d %H:%M:%S", &t);

    JsonDocument doc;
    if (message) doc["message"] = message;
    doc["epoch"] = (int64_t)now;
    doc["local"] = local;
    doc["timezone"] = Services::loadSettings().timezone;
    doc["valid"] = st.valid;
    doc["source"] = sourceName(st.source);
    if (st.last_sync) doc["last_ntp_sync"] = st.last_sync;
    JsonObject ntp = doc["ntp"].to<JsonObject>();
    ntp["syncing"] = st.syncing;
    if (!st.server.empty()) ntp["server"] = st.server;
    ntp["failures"] = st.failures;
    if (st.next_attempt_s) ntp["next_attempt_s"] = st.next_attempt_s;
    return Http::sendJson(req, 200, doc);
}

esp_err_t getHandler(httpd_req_t* req) {
    return sendTime(req);
}

esp_err_t setHandler(httpd_req_t* req) {
    std::string body;
    JsonDocument in;
    if (req->content_len == 0 || !Http::readBody(req, body, 256) || deserializeJson(in, body) ||
        !in.is<JsonObject>() || !in["epoch"].is<double>()) {
        return Http::sendError(req, 400, "Body must be {\"epoch\": <UTC seconds>, \"timezone\"?: \"<POSIX TZ>\"}");
    }
    if (!in["timezone"].isNull()) {
        if (!in["timezone"].is<const char*>() || !in["timezone"].as<const char*>()[0]) {
            return Http::sendError(req, 400, "timezone must be a POSIX TZ string, e.g. \"IST-5:30\"");
        }
        ndb::system::Settings s;
        s.timezone = in["timezone"].as<const char*>();
        if (!Services::saveSettings(s, ndb::system::Settings::F_TIMEZONE)) return Http::sendError(req, 503, "Settings database unavailable");
        TimeSyncHelper::applyTimezone(s.timezone);
    }
    const double epoch = in["epoch"].as<double>();
    if (!std::isfinite(epoch) || !TimeSyncHelper::instance().setTime((int64_t)epoch)) {
        return Http::sendError(req, 400, "epoch must be UTC seconds between 2021 and 2100");
    }
    return sendTime(req, "Clock set");
}

esp_err_t syncHandler(httpd_req_t* req) {
    TimeSyncHelper::instance().requestSync();
    return sendTime(req, "Sync started");
}

} // namespace

void Routes::registerTime(Http::Server& server) {
    server.on("/api/time", HTTP_GET, getHandler);
    server.on("/api/time", HTTP_POST, setHandler);
    server.on("/api/time/sync", HTTP_POST, syncHandler);
}
