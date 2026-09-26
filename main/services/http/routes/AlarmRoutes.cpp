// /api/alarms: the alarm list (alarms.json until step B) and ringing control.
//
//   GET    /api/alarms           list
//   POST   /api/alarms           create or update {"id"?, "hour", "minute", "tone_file"?, "enabled"?}
//   DELETE /api/alarms?id=N
//   GET    /api/alarms/status    ringing state
//   POST   /api/alarms/ring      test ring now {"tone"?, "ring_limit_s"?, "snooze_s"?}
//   POST   /api/alarms/snooze
//   POST   /api/alarms/stop
#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "services/alarm/AlarmService.h"

#include <cstring>

using Services::Alarm;
using Services::AlarmRing;
using Services::AlarmService;

namespace {

// Empty = the built-in tone; otherwise a library song id.
constexpr const char* DEFAULT_TONE = "";
constexpr uint32_t REQUEST_TIMEOUT_MS = 5000;

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

const char* stateName(AlarmRing::State s) {
    return s == AlarmRing::State::Ringing ? "ringing" : s == AlarmRing::State::Snoozed ? "snoozed" : "idle";
}

void addStatus(JsonObject o) {
    AlarmService::Status st = AlarmService::getInstance().status();
    o["state"] = stateName(st.state);
    o["time_synced"] = st.time_synced;
    if (st.last_end != AlarmRing::EndReason::None) {
        o["last_end"] = st.last_end == AlarmRing::EndReason::TimedOut ? "timed_out" : "stopped";
    }
    if (st.state == AlarmRing::State::Idle) return;
    o["alarm_id"] = st.alarm_id;
    o["tone"] = st.tone;
    if (!st.tone_title.empty()) o["tone_title"] = st.tone_title;
    if (st.source != AlarmRing::Source::None) {
        o["playing"] = st.source == AlarmRing::Source::Song ? "song" : "builtin";
    }
    if (!st.fallback_reason.empty()) o["fallback_reason"] = st.fallback_reason;
    o["ringing_s"] = st.ringing_ms / 1000;
    if (st.state == AlarmRing::State::Snoozed) o["snooze_left_s"] = (st.snooze_left_ms + 999) / 1000;
    o["snoozes"] = st.snoozes;
}

esp_err_t sendStatus(httpd_req_t* req) {
    JsonDocument doc;
    addStatus(doc.to<JsonObject>());
    return Http::sendJson(req, 200, doc);
}

esp_err_t statusHandler(httpd_req_t* req) {
    return sendStatus(req);
}

esp_err_t ringHandler(httpd_req_t* req) {
    AlarmService::RingOptions opts;
    if (req->content_len > 0) {
        std::string body;
        JsonDocument in;
        if (!Http::readBody(req, body, 512) || deserializeJson(in, body) || !in.is<JsonObject>()) {
            return Http::sendError(req, 400, "Body must be {\"tone\"?, \"ring_limit_s\"?, \"snooze_s\"?}");
        }
        if (!in["tone"].isNull()) {
            if (!in["tone"].is<const char*>() || strlen(in["tone"].as<const char*>()) >= sizeof(Alarm::tone_file)) {
                return Http::sendError(req, 400, "tone must be a library song id");
            }
            opts.tone = in["tone"].as<const char*>();
        }
        // Shorter limits for testing snooze and timeout.
        int limit_s = in["ring_limit_s"] | 0;
        int snooze_s = in["snooze_s"] | 0;
        if (limit_s < 0 || limit_s > 3600 || snooze_s < 0 || snooze_s > 3600) {
            return Http::sendError(req, 400, "ring_limit_s and snooze_s must be 0-3600");
        }
        opts.ring_limit_ms = (uint32_t)limit_s * 1000;
        opts.snooze_ms = (uint32_t)snooze_s * 1000;
    }
    if (!AlarmService::getInstance().ring(opts, REQUEST_TIMEOUT_MS)) return Http::sendError(req, 504, "Alarm task busy");
    return sendStatus(req);
}

esp_err_t actionHandler(httpd_req_t* req) {
    std::string action(req->uri + strlen("/api/alarms/"));
    action = action.substr(0, action.find('?'));
    auto& svc = AlarmService::getInstance();
    bool ok;
    if (action == "stop") ok = svc.stopActiveAlarm(true, REQUEST_TIMEOUT_MS);
    else if (action == "snooze") ok = svc.snooze(REQUEST_TIMEOUT_MS);
    else if (action == "ring") return ringHandler(req);
    else return Http::sendError(req, 404, "Unknown alarm action");
    if (!ok) return Http::sendError(req, 504, "Alarm task busy");
    return sendStatus(req);
}

} // namespace

void Routes::registerAlarms(Http::Server& server) {
    server.on("/api/alarms", HTTP_GET, listHandler);
    server.on("/api/alarms", HTTP_POST, saveHandler);
    server.on("/api/alarms", HTTP_DELETE, deleteHandler);
    server.on("/api/alarms/status", HTTP_GET, statusHandler);
    server.on("/api/alarms/*", HTTP_POST, actionHandler);
}
