#include "services/alarm/AlarmTools.h"

#include "app/media_player/NexusPlayer.h"
#include "media_player/CatalogDB.h"
#include "services/alarm/AlarmSchedule.h"
#include "services/alarm/AlarmService.h"
#include "services/time/TimeSyncHelper.h"
#include "esp_log.h"

#include <ctime>
#include <string>

namespace Services {
namespace {

const char* const TAG = "AlarmTools";
constexpr uint32_t REQUEST_TIMEOUT_MS = 3000;
constexpr int MAX_TIMER_S = 24 * 3600;
constexpr int MAX_REMINDER_MINUTES = 7 * 24 * 60;

using GeminiSkills::DecodedSkillCall;
using GeminiSkills::SkillType;

// "Mon 28 Sep 07:00", local time.
std::string localText(int64_t epoch) {
    time_t t = (time_t)epoch;
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%a %d %b %H:%M", &tm);
    return buf;
}

std::string clockText(int hour, int minute) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    return buf;
}

void error(JsonDocument& r, const char* message) {
    r["status"] = "error";
    r["message"] = message;
}

// Tells the model the current local time, so it can speak relative times.
void addNow(JsonDocument& r) {
    r["now"] = localText(time(nullptr));
}

// A library song whose file is on the card, by title or artist; empty if none.
std::string findTone(const std::string& query, std::string& title) {
    if (query.empty()) return "";
    auto& storage = NexusPlayer::getInstance().getStorageManager();
    for (const LibraryTrack& t : CatalogDB::getInstance().search(query.c_str())) {
        if (storage.fileExists(t.id.c_str())) {
            title = t.doc.title;
            return t.id;
        }
    }
    return "";
}

void alarmToJson(int id, const AlarmDoc& a, JsonObject o, int64_t now) {
    o["id"] = id;
    if (a.kind == 1) {
        o["type"] = "timer";
        if (a.at > now) o["remaining_s"] = (int64_t)a.at - now;
    } else {
        o["type"] = "alarm";
        if (a.at) {
            o["time"] = localText(a.at);
        } else {
            o["time"] = clockText(a.hour, a.minute);
            o["repeat"] = formatDays(a.days);
        }
    }
    if (!a.label.empty()) o["label"] = a.label;
    o["enabled"] = a.enabled;
    if (!a.tone.empty()) {
        ndb::music::TrackDoc rec;
        if (CatalogDB::getInstance().get(a.tone.c_str(), rec)) {
            o["tone"] = rec.title;
        } else {
            o["tone"] = a.tone;
        }
    }
    const int64_t next = AlarmService::nextFireOf(a, now);
    if (next) o["next"] = localText(next);
}

// Hour/minute/days/day into the "when" fields. Returns an error message, or nullptr.
template <typename Doc>
const char* setWhen(Doc& doc, int hour, int minute, const std::string& days_text, const std::string& day, int64_t now) {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return "hour must be 0-23 and minute 0-59";
    uint8_t days = 0;
    if (!parseDays(days_text, days)) return "days not understood; use 'once', 'daily', 'weekdays', 'weekends' or day names";
    doc.hour = (uint8_t)hour;
    doc.minute = (uint8_t)minute;
    doc.days = days;
    doc.at = 0;
    if (!day.empty()) {
        if (days) return "give either repeat days or a day, not both";
        const int64_t at = resolveDay(day, hour, minute, now);
        if (at == DAY_INVALID) return "day not understood; use 'today', 'tomorrow', a day name or YYYY-MM-DD";
        if (at == DAY_PAST) return "that time has already passed";
        doc.at = (uint32_t)at;
    }
    return nullptr;
}

void setAlarm(const GeminiSkills::set_alarm_args_t& args, JsonDocument& r) {
    auto& svc = AlarmService::getInstance();
    const int64_t now = time(nullptr);
    AlarmDoc a;
    if (const char* err = setWhen(a, args.hour, args.minute, args.days, args.day, now)) return error(r, err);
    a.label = args.label.substr(0, 64);
    a.enabled = args.enabled;

    std::string tone_title;
    if (!args.tone.empty()) {
        a.tone = findTone(args.tone, tone_title);
        if (a.tone.empty()) r["note"] = "No downloaded song matches '" + args.tone + "'; the built-in tone will ring.";
    }

    // The same time and days as an existing alarm: update it.
    int id = 0;
    for (const auto& [eid, e] : svc.alarms()) {
        if (e.kind == 0 && e.hour == a.hour && e.minute == a.minute && e.days == a.days && e.at == a.at) {
            id = eid;
            if (args.label.empty()) a.label = e.label;
            if (args.tone.empty()) a.tone = e.tone;
            a.snooze_min = e.snooze_min;
            a.volume = e.volume;
            break;
        }
    }
    ESP_LOGI(TAG, "set_alarm %s %s%s", clockText(a.hour, a.minute).c_str(), formatDays(a.days).c_str(),
             id ? " (update)" : "");
    id = svc.saveAlarm(id, a);
    if (!id) return error(r, "Could not save the alarm");
    r["status"] = "success";
    alarmToJson(id, a, r["alarm"].to<JsonObject>(), now);
    if (!tone_title.empty()) r["alarm"]["tone"] = tone_title;
    if (!TimeSyncHelper::clockValid()) r["warning"] = "The device clock is not set yet; alarms wait for it.";
    addNow(r);
}

void setTimer(const GeminiSkills::set_timer_args_t& args, JsonDocument& r) {
    if (args.seconds <= 0 || args.seconds > MAX_TIMER_S) return error(r, "seconds must be 1 to 86400");
    const int64_t now = time(nullptr);
    if (!TimeSyncHelper::clockValid()) return error(r, "The device clock is not set yet");
    AlarmDoc a;
    a.kind = 1;
    a.at = (uint32_t)(now + args.seconds);
    a.label = args.label.substr(0, 64);
    const int id = AlarmService::getInstance().saveAlarm(0, a);
    if (!id) return error(r, "Could not start the timer");
    ESP_LOGI(TAG, "set_timer %d s -> timer %d", args.seconds, id);
    r["status"] = "success";
    alarmToJson(id, a, r["timer"].to<JsonObject>(), now);
    r["timer"]["ends"] = localText(a.at);
    addNow(r);
}

void reminderToJson(int id, const ReminderDoc& d, JsonObject o, int64_t now) {
    o["id"] = id;
    o["text"] = d.text;
    o["kind"] = d.action ? "action" : "reminder";
    if (d.at) {
        o["time"] = localText(d.at);
    } else {
        o["time"] = clockText(d.hour, d.minute);
        o["repeat"] = formatDays(d.days);
    }
    o["enabled"] = d.enabled;
    if (d.pending) o["pending"] = true;
    const int64_t next = AlarmService::nextFireOf(d, now);
    if (next) o["next"] = localText(next);
}

void schedule(const GeminiSkills::schedule_args_t& args, JsonDocument& r) {
    if (args.text.empty()) return error(r, "text is required");
    if (args.text.size() > AlarmService::MAX_REMINDER_TEXT) return error(r, "text is too long");
    if (!TimeSyncHelper::clockValid()) return error(r, "The device clock is not set yet");
    const int64_t now = time(nullptr);
    ReminderDoc d;
    d.text = args.text;
    d.action = args.kind == "action";
    if (args.in_minutes > 0) {
        if (args.in_minutes > MAX_REMINDER_MINUTES) return error(r, "in_minutes must be at most a week");
        d.at = (uint32_t)(now + (int64_t)args.in_minutes * 60);
    } else if (const char* err = setWhen(d, args.hour, args.minute, args.days, args.day, now)) {
        return error(r, err);
    }
    // The model sometimes repeats a call; the same reminder is not added twice.
    for (const auto& [eid, e] : AlarmService::getInstance().reminders()) {
        if (e.enabled && e.text == d.text && e.action == d.action && e.hour == d.hour && e.minute == d.minute && e.days == d.days &&
            (e.at == d.at || (d.at && e.at && (e.at > d.at ? e.at - d.at : d.at - e.at) < 120))) {
            r["status"] = "success";
            r["message"] = "This item already exists";
            reminderToJson(eid, e, r["scheduled"].to<JsonObject>(), now);
            addNow(r);
            return;
        }
    }
    const int id = AlarmService::getInstance().saveReminder(0, d);
    if (!id) return error(r, "Could not save the item");
    ESP_LOGI(TAG, "schedule -> %s %d", d.action ? "action" : "reminder", id);
    r["status"] = "success";
    reminderToJson(id, d, r["scheduled"].to<JsonObject>(), now);
    addNow(r);
}

// Alarms, timers and reminders in one reply (list_schedule).
void listSchedule(JsonDocument& r) {
    const int64_t now = time(nullptr);
    r["status"] = "success";
    JsonArray alarms = r["alarms"].to<JsonArray>();
    for (const auto& [id, a] : AlarmService::getInstance().alarms()) alarmToJson(id, a, alarms.add<JsonObject>(), now);
    const AlarmService::Status st = AlarmService::getInstance().status();
    if (st.state == AlarmRing::State::Ringing) r["ringing"] = st.alarm_id;
    if (st.state == AlarmRing::State::Snoozed) r["snoozed"] = st.alarm_id;
    JsonArray reminders = r["scheduled"].to<JsonArray>();
    for (const auto& [id, d] : AlarmService::getInstance().reminders()) reminderToJson(id, d, reminders.add<JsonObject>(), now);
    addNow(r);
}

// A due item's own turn runs unattended; it may not change the schedule.
bool scheduledTurnRefusal(JsonDocument& r) {
    error(r, "The schedule can't be changed while carrying out a scheduled item; the user can ask afterwards");
    return true;
}

} // namespace

bool handleAlarmTool(const DecodedSkillCall& call, JsonDocument& r) {
    auto& svc = AlarmService::getInstance();
    switch (call.type) {
    case SkillType::RINGING_ALARM: {
        const std::string& action = call.args.ringing_alarm->action;
        if (action == "snooze") {
            if (svc.status().state != AlarmRing::State::Ringing) {
                error(r, "No alarm is ringing");
                return true;
            }
            svc.snooze(REQUEST_TIMEOUT_MS);
            const AlarmService::Status st = svc.status();
            r["status"] = "success";
            r["snooze_minutes"] = (st.snooze_left_ms + 59999) / 60000;
            return true;
        }
        const bool active = svc.status().state != AlarmRing::State::Idle;
        if (active) svc.stopActiveAlarm(true, REQUEST_TIMEOUT_MS);
        r["status"] = "success";
        r["message"] = active ? "Alarm stopped" : "No alarm was ringing";
        return true;
    }
    case SkillType::SET_ALARM:
        if (call.args.set_alarm) setAlarm(*call.args.set_alarm, r);
        return true;
    case SkillType::LIST_SCHEDULE:
        listSchedule(r);
        return true;
    case SkillType::CANCEL_SCHEDULED: {
        if (svc.inScheduledTurn()) return scheduledTurnRefusal(r);
        const auto* args = call.args.cancel_scheduled;
        bool reminder = args->kind == "reminder";
        if (reminder ? svc.deleteReminder(args->id) : svc.deleteAlarm(args->id)) {
            r["status"] = "success";
            r["message"] = "Deleted";
        } else {
            error(r, reminder ? "No reminder with that id" : "No alarm or timer with that id");
        }
        return true;
    }
    case SkillType::SET_TIMER:
        if (call.args.set_timer) setTimer(*call.args.set_timer, r);
        return true;
    case SkillType::SCHEDULE:
        if (svc.inScheduledTurn()) return scheduledTurnRefusal(r);
        if (call.args.schedule) schedule(*call.args.schedule, r);
        return true;
    case SkillType::ACKNOWLEDGE_REMINDERS: {
        int cleared = 0;
        for (const auto& [id, d] : svc.reminders()) {
            if (d.pending && svc.acknowledgeReminder(id)) ++cleared;
        }
        r["status"] = "success";
        r["cleared"] = cleared;
        return true;
    }
    default:
        return false;
    }
}

} // namespace Services
