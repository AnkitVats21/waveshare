// /api/alarms: the alarms in system.ndb and ringing control.
//
//   GET    /api/alarms           list
//   POST   /api/alarms           create, or update the one with "id" (fields left out keep their values)
//                                {"id"?, "hour", "minute", "days"?, "at"?, "label"?, "enabled"?,
//                                 "tone"?, "snooze_min"?, "volume"?, "kind"?}
//                                a new alarm with neither days nor at repeats daily
//   DELETE /api/alarms?id=N
//                                tone: "" or "builtin[:<name>]", "file:<name>" (in /sdcard/media/alarm),
//                                or a library song id (docs/alarm-design.md, "Tones")
//   GET    /api/alarms/tones     {"builtin": [names], "dir", "files": [{"name", "bytes"}]}
//   GET    /api/alarms/status    ringing state
//   POST   /api/alarms/ring      test ring now {"tone"?, "ring_limit_s"?, "snooze_s"?}
//   POST   /api/alarms/snooze
//   POST   /api/alarms/stop
//
// /api/reminders: the scheduled items in system.ndb ("action": true for an
// instruction Gemini carries out, false for a reminder it tells the user).
//
//   GET    /api/reminders        list
//   POST   /api/reminders        create, or update the one with "id"
//                                {"id"?, "hour", "minute", "days"?, "at"?, "text", "enabled"?}
//   DELETE /api/reminders?id=N
//   POST   /api/reminders/ack?id=N   clear pending
#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "services/alarm/AlarmService.h"
#include "audio_core/AlertPlayer.h"
#include "sd_storage/Fs.h"

#include <cstring>
#include <ctime>

using Services::AlarmDoc;
using Services::AlarmTone;
using Services::AlarmRing;
using Services::AlarmService;
using Services::ReminderDoc;

namespace {

constexpr uint32_t REQUEST_TIMEOUT_MS = 5000;

void toJson(int id, const AlarmDoc& a, JsonObject out) {
    out["id"] = id;
    out["hour"] = a.hour;
    out["minute"] = a.minute;
    out["days"] = a.days;
    if (a.at) out["at"] = a.at;
    out["label"] = a.label;
    out["enabled"] = a.enabled;
    out["tone"] = a.tone;
    out["snooze_min"] = a.snooze_min;
    out["volume"] = a.volume;
    out["kind"] = a.kind == 1 ? "timer" : "alarm";
    if (a.last_fired) out["last_fired"] = a.last_fired;
    if (a.snooze_until) out["snooze_until"] = a.snooze_until;
    const int64_t next = AlarmService::nextFireOf(a, time(nullptr));
    if (next) out["next_fire"] = next;
}

esp_err_t listHandler(httpd_req_t* req) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& [id, a] : AlarmService::getInstance().alarms()) {
        toJson(id, a, arr.add<JsonObject>());
    }
    return Http::sendJson(req, 200, doc);
}

// Integer field in [lo, hi] if present; false if present but invalid.
template <typename T>
bool takeInt(JsonObject in, const char* key, int64_t lo, int64_t hi, T& dst) {
    JsonVariant v = in[key];
    if (v.isNull()) return true;
    if (!v.is<int64_t>()) return false;
    const int64_t n = v.as<int64_t>();
    if (n < lo || n > hi) return false;
    dst = (T)n;
    return true;
}

// Null if the tone setting is usable; else why not. A tone that is valid but
// missing (a deleted file or song) is accepted: it rings the built-in tone.
const char* toneError(const char* tone) {
    if (strlen(tone) > AlarmService::MAX_TONE_LEN) return "tone too long";
    const AlarmTone t = Services::parseTone(tone);
    switch (t.kind) {
    case AlarmTone::Kind::Builtin:
        return t.value.empty() || AlertPlayer::alarmToneIndex(t.value.c_str()) >= 0 ? nullptr : "unknown built-in tone";
    case AlarmTone::Kind::File:
        return Services::isValidToneFileName(t.value) ? nullptr : "tone file must be a plain .ogg, .opus or .webm name";
    default:
        return strchr(tone, '/') ? "tone must be builtin[:name], file:<name> or a library song id" : nullptr;
    }
}

esp_err_t saveHandler(httpd_req_t* req) {
    std::string body;
    if (!Http::readBody(req, body, 1024)) return Http::sendError(req, 400, "Missing or oversized body");
    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc.is<JsonObject>()) return Http::sendError(req, 400, "Body must be a JSON object");
    JsonObject in = doc.as<JsonObject>();

    auto& svc = AlarmService::getInstance();
    int id = in["id"] | 0;
    AlarmDoc a;
    const bool exists = id > 0 && Services::loadAlarm(id, a);
    if (id > 0 && !exists) return Http::sendError(req, 404, "No such alarm");

    if (!takeInt(in, "hour", 0, 23, a.hour) || !takeInt(in, "minute", 0, 59, a.minute)) {
        return Http::sendError(req, 400, "hour must be 0-23 and minute 0-59");
    }
    if (!takeInt(in, "days", 0, 0x7F, a.days)) return Http::sendError(req, 400, "days must be 0-127 (bit0 Mon .. bit6 Sun)");
    if (!takeInt(in, "at", 0, 0xFFFFFFFFLL, a.at)) return Http::sendError(req, 400, "at must be epoch seconds");
    if (!takeInt(in, "snooze_min", 1, 60, a.snooze_min)) return Http::sendError(req, 400, "snooze_min must be 1-60");
    if (!takeInt(in, "volume", 0, 100, a.volume)) return Http::sendError(req, 400, "volume must be 0-100");
    if (!exists && in["hour"].isNull() && !a.at) return Http::sendError(req, 400, "hour and minute, or at, are required");
    // Alarms used to be daily only, and the dashboard does not send days yet.
    if (!exists && in["days"].isNull() && !a.at) a.days = Services::AlarmWhen::EVERY_DAY;
    if (!in["kind"].isNull()) {
        const char* kind = in["kind"] | "";
        if (strcmp(kind, "alarm") && strcmp(kind, "timer")) return Http::sendError(req, 400, "kind must be alarm or timer");
        a.kind = strcmp(kind, "timer") == 0 ? 1 : 0;
    }
    if (a.kind == 1 && !a.at) return Http::sendError(req, 400, "a timer needs at");
    if (!in["enabled"].isNull()) a.enabled = in["enabled"].as<bool>();
    if (in["label"].is<const char*>()) a.label = in["label"].as<const char*>();
    // "tone_file" is the old name; a path (an old .wav tone) means the built-in tone.
    JsonVariant tone = in["tone"].isNull() ? in["tone_file"] : in["tone"];
    if (!tone.isNull()) {
        if (!tone.is<const char*>()) return Http::sendError(req, 400, "tone must be a string");
        a.tone = tone.as<const char*>();
        if (!a.tone.empty() && a.tone[0] == '/') a.tone.clear();
        if (const char* err = toneError(a.tone.c_str())) return Http::sendError(req, 400, err);
    }
    if (a.label.size() > 64) return Http::sendError(req, 400, "label too long");

    id = svc.saveAlarm(id, a);
    if (!id) return Http::sendError(req, 500, "Could not save the alarm");
    Services::loadAlarm(id, a);
    JsonDocument out;
    toJson(id, a, out.to<JsonObject>());
    return Http::sendJson(req, 200, out);
}

esp_err_t deleteHandler(httpd_req_t* req) {
    std::string id_str;
    if (!Http::queryParam(req, "id", id_str) || id_str.empty()) return Http::sendError(req, 400, "Missing id");
    if (!AlarmService::getInstance().deleteAlarm(atoi(id_str.c_str()))) return Http::sendError(req, 404, "No such alarm");
    return Http::sendOk(req, "Alarm deleted");
}

void reminderToJson(int id, const ReminderDoc& r, JsonObject out) {
    out["id"] = id;
    out["hour"] = r.hour;
    out["minute"] = r.minute;
    out["days"] = r.days;
    if (r.at) out["at"] = r.at;
    out["text"] = r.text;
    out["action"] = r.action;
    out["enabled"] = r.enabled;
    out["pending"] = r.pending;
    if (r.last_fired) out["last_fired"] = r.last_fired;
    const int64_t next = AlarmService::nextFireOf(r, time(nullptr));
    if (next) out["next_fire"] = next;
}

esp_err_t reminderListHandler(httpd_req_t* req) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& [id, r] : AlarmService::getInstance().reminders()) {
        reminderToJson(id, r, arr.add<JsonObject>());
    }
    return Http::sendJson(req, 200, doc);
}

esp_err_t reminderSaveHandler(httpd_req_t* req) {
    std::string body;
    if (!Http::readBody(req, body, 1024)) return Http::sendError(req, 400, "Missing or oversized body");
    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc.is<JsonObject>()) return Http::sendError(req, 400, "Body must be a JSON object");
    JsonObject in = doc.as<JsonObject>();

    int id = in["id"] | 0;
    ReminderDoc r;
    const bool exists = id > 0 && Services::loadReminder(id, r);
    if (id > 0 && !exists) return Http::sendError(req, 404, "No such reminder");

    if (!takeInt(in, "hour", 0, 23, r.hour) || !takeInt(in, "minute", 0, 59, r.minute)) {
        return Http::sendError(req, 400, "hour must be 0-23 and minute 0-59");
    }
    if (!takeInt(in, "days", 0, 0x7F, r.days)) return Http::sendError(req, 400, "days must be 0-127 (bit0 Mon .. bit6 Sun)");
    if (!takeInt(in, "at", 0, 0xFFFFFFFFLL, r.at)) return Http::sendError(req, 400, "at must be epoch seconds");
    if (!exists && in["hour"].isNull() && !r.at) return Http::sendError(req, 400, "hour and minute, or at, are required");
    if (!in["enabled"].isNull()) r.enabled = in["enabled"].as<bool>();
    if (!in["action"].isNull()) r.action = in["action"].as<bool>();
    if (!in["text"].isNull()) {
        if (!in["text"].is<const char*>()) return Http::sendError(req, 400, "text must be a string");
        r.text = in["text"].as<const char*>();
    }
    if (r.text.empty()) return Http::sendError(req, 400, "text is required");
    if (r.text.size() > AlarmService::MAX_REMINDER_TEXT) return Http::sendError(req, 400, "text too long");

    id = AlarmService::getInstance().saveReminder(id, r);
    if (!id) return Http::sendError(req, 500, "Could not save the reminder");
    Services::loadReminder(id, r);
    JsonDocument out;
    reminderToJson(id, r, out.to<JsonObject>());
    return Http::sendJson(req, 200, out);
}

esp_err_t reminderDeleteHandler(httpd_req_t* req) {
    std::string id_str;
    if (!Http::queryParam(req, "id", id_str) || id_str.empty()) return Http::sendError(req, 400, "Missing id");
    if (!AlarmService::getInstance().deleteReminder(atoi(id_str.c_str()))) return Http::sendError(req, 404, "No such reminder");
    return Http::sendOk(req, "Reminder deleted");
}

esp_err_t reminderActionHandler(httpd_req_t* req) {
    std::string action(req->uri + strlen("/api/reminders/"));
    action = action.substr(0, action.find('?'));
    if (action != "ack") return Http::sendError(req, 404, "Unknown reminder action");
    std::string id_str;
    if (!Http::queryParam(req, "id", id_str) || id_str.empty()) return Http::sendError(req, 400, "Missing id");
    if (!AlarmService::getInstance().acknowledgeReminder(atoi(id_str.c_str()))) {
        return Http::sendError(req, 404, "No such reminder");
    }
    return Http::sendOk(req, "Reminder acknowledged");
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
            if (!in["tone"].is<const char*>()) return Http::sendError(req, 400, "tone must be a string");
            if (const char* err = toneError(in["tone"].as<const char*>())) return Http::sendError(req, 400, err);
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

// The tones an alarm can use: built-in patterns and the files uploaded to
// ALARM_TONE_DIR (library songs come from /api/music/library).
bool listToneFile(const sd_storage::DirEntry& e, void* ctx) {
    if (e.is_dir || !Services::isValidToneFileName(e.name)) return true;
    JsonObject f = static_cast<JsonArray*>(ctx)->add<JsonObject>();
    f["name"] = e.name;
    f["bytes"] = e.size;
    return true;
}

esp_err_t tonesHandler(httpd_req_t* req) {
    JsonDocument doc;
    JsonArray builtin = doc["builtin"].to<JsonArray>();
    for (size_t i = 0; i < AlertPlayer::alarmToneCount(); ++i) builtin.add(AlertPlayer::alarmToneName(i));
    doc["dir"] = Services::ALARM_TONE_DIR;
    JsonArray files = doc["files"].to<JsonArray>();
    if (sd_storage::Fs::isDir(Services::ALARM_TONE_DIR)) {
        sd_storage::Fs::list(Services::ALARM_TONE_DIR, nullptr, true, listToneFile, &files);
    }
    return Http::sendJson(req, 200, doc);
}

} // namespace

void Routes::registerAlarms(Http::Server& server) {
    server.on("/api/alarms", HTTP_GET, listHandler);
    server.on("/api/alarms", HTTP_POST, saveHandler);
    server.on("/api/alarms", HTTP_DELETE, deleteHandler);
    server.on("/api/alarms/status", HTTP_GET, statusHandler);
    server.on("/api/alarms/tones", HTTP_GET, tonesHandler);
    server.on("/api/alarms/*", HTTP_POST, actionHandler);
    server.on("/api/reminders", HTTP_GET, reminderListHandler);
    server.on("/api/reminders", HTTP_POST, reminderSaveHandler);
    server.on("/api/reminders", HTTP_DELETE, reminderDeleteHandler);
    server.on("/api/reminders/*", HTTP_POST, reminderActionHandler);
}
