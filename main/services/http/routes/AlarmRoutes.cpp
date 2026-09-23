#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "services/alarm/AlarmService.h"

#include <cstring>

using Services::Alarm;
using Services::AlarmService;

namespace {

constexpr const char* DEFAULT_TONE = "/sdcard/alarms/soft_wake_up.wav";

void toJson(const Alarm& a, JsonObject out) {
    out["id"] = a.id;
    out["hour"] = a.hour;
    out["minute"] = a.minute;
    out["tone_file"] = std::string(a.tone_file);  // copy: a char array would be stored by pointer
    out["enabled"] = a.enabled;
}

esp_err_t listHandler(httpd_req_t* req) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const Alarm& a : AlarmService::getInstance().getAlarms()) {
        toJson(a, arr.add<JsonObject>());
    }
    return Http::sendJson(req, 200, doc);
}

// Creates an alarm, or updates the one with the given "id".
// Body: {"id"?, "hour", "minute", "tone_file"?, "enabled"?}
esp_err_t saveHandler(httpd_req_t* req) {
    std::string body;
    if (!Http::readBody(req, body, 512)) return Http::sendError(req, 400, "Missing or oversized body");
    JsonDocument in;
    if (deserializeJson(in, body) || !in.is<JsonObject>()) return Http::sendError(req, 400, "Body must be a JSON object");

    int hour = in["hour"] | -1;
    int minute = in["minute"] | -1;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return Http::sendError(req, 400, "hour (0-23) and minute (0-59) are required");
    }
    const char* tone = in["tone_file"] | DEFAULT_TONE;
    if (tone[0] == '\0') tone = DEFAULT_TONE;
    if (strlen(tone) >= sizeof(Alarm::tone_file)) return Http::sendError(req, 400, "tone_file path too long");

    auto& svc = AlarmService::getInstance();
    int id = in["id"] | 0;
    if (id <= 0) {
        for (const Alarm& a : svc.getAlarms()) id = (a.id > id) ? a.id : id;
        ++id;
    }

    Alarm alarm = {};
    alarm.id = id;
    alarm.hour = hour;
    alarm.minute = minute;
    strncpy(alarm.tone_file, tone, sizeof(alarm.tone_file) - 1);
    alarm.enabled = in["enabled"] | true;
    svc.addOrUpdateAlarm(alarm);

    JsonDocument out;
    toJson(alarm, out.to<JsonObject>());
    return Http::sendJson(req, 200, out);
}

esp_err_t deleteHandler(httpd_req_t* req) {
    std::string id_str;
    if (!Http::queryParam(req, "id", id_str) || id_str.empty()) return Http::sendError(req, 400, "Missing id");
    AlarmService::getInstance().deleteAlarm(atoi(id_str.c_str()));
    return Http::sendOk(req, "Alarm deleted");
}

esp_err_t stopHandler(httpd_req_t* req) {
    AlarmService::getInstance().stopActiveAlarm();
    return Http::sendOk(req, "Alarm stopped");
}

} // namespace

void Routes::registerAlarms(Http::Server& server) {
    server.on("/api/alarms", HTTP_GET, listHandler);
    server.on("/api/alarms", HTTP_POST, saveHandler);
    server.on("/api/alarms", HTTP_DELETE, deleteHandler);
    server.on("/api/alarms/stop", HTTP_POST, stopHandler);
}
