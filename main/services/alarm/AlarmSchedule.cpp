#include "services/alarm/AlarmSchedule.h"

#include <cctype>
#include <cstdio>
#include <ctime>

namespace Services {

int64_t nextFire(const AlarmWhen& when, int64_t after) {
    if (when.at != 0) return int64_t(when.at) > after ? int64_t(when.at) : 0;
    if (when.hour > 23 || when.minute > 59) return 0;

    // Walk calendar days rather than adding 24 h, so DST days (23 or 25 h)
    // keep the local time. Eight days covers every weekday once.
    time_t base = time_t(after);
    struct tm day;
    localtime_r(&base, &day);
    for (int i = 0; i < 8; ++i) {
        struct tm t = {};
        t.tm_year = day.tm_year;
        t.tm_mon = day.tm_mon;
        t.tm_mday = day.tm_mday + i;
        t.tm_hour = when.hour;
        t.tm_min = when.minute;
        t.tm_isdst = -1;
        const time_t fire = mktime(&t);   // normalises tm, including tm_wday
        if (fire == time_t(-1)) return 0;
        if (int64_t(fire) <= after) continue;
        if (when.days != 0 && !(when.days & dayBit(t.tm_wday))) continue;
        return int64_t(fire);
    }
    return 0;
}

namespace {

const char* const DAY_LABELS[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

std::string lower(const std::string& s) {
    std::string out;
    for (char c : s) out += (char)std::tolower((unsigned char)c);
    return out;
}

// Index 0 (Mon) .. 6 (Sun) of a day name or its prefix ("tues", "thursdays"); -1 if none.
int dayIndex(std::string w) {
    if (w.size() > 3 && w.back() == 's') w.pop_back();   // "mondays"
    static const char* const FULL[7] = {"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"};
    if (w.size() < 3) return -1;
    for (int i = 0; i < 7; ++i) {
        if (std::string(FULL[i]).compare(0, w.size(), w) == 0) return i;
    }
    return -1;
}

time_t localTime(int year, int mon, int mday, int hour, int minute) {
    struct tm t = {};
    t.tm_year = year;
    t.tm_mon = mon;
    t.tm_mday = mday;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_isdst = -1;
    return mktime(&t);
}

} // namespace

bool parseDays(const std::string& text, uint8_t& days) {
    const std::string s = lower(text);
    uint8_t out = 0;
    std::string word;
    auto take = [&](const std::string& w) {
        if (w.empty() || w == "and" || w == "every" || w == "on") return true;
        if (w == "once" || w == "one-time" || w == "none") return true;
        if (w == "daily" || w == "everyday" || w == "day" || w == "days") { out |= AlarmWhen::EVERY_DAY; return true; }
        if (w == "weekdays" || w == "weekday") { out |= 0x1F; return true; }
        if (w == "weekends" || w == "weekend") { out |= 0x60; return true; }
        const int i = dayIndex(w);
        if (i < 0) return false;
        out |= uint8_t(1u << i);
        return true;
    };
    for (char c : s) {
        if (c == ',' || c == ' ' || c == '/' || c == '+' || c == '&') {
            if (!take(word)) return false;
            word.clear();
        } else {
            word += c;
        }
    }
    if (!take(word)) return false;
    days = out;
    return true;
}

std::string formatDays(uint8_t days) {
    days &= AlarmWhen::EVERY_DAY;
    if (days == 0) return "once";
    if (days == AlarmWhen::EVERY_DAY) return "every day";
    if (days == 0x1F) return "weekdays";
    if (days == 0x60) return "weekends";
    std::string out;
    for (int i = 0; i < 7; ++i) {
        if (!(days & (1u << i))) continue;
        if (!out.empty()) out += ", ";
        out += DAY_LABELS[i];
    }
    return out;
}

int64_t resolveDay(const std::string& day_text, int hour, int minute, int64_t now) {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return DAY_INVALID;
    const std::string day = lower(day_text);
    time_t base = time_t(now);
    struct tm today;
    localtime_r(&base, &today);

    time_t t;
    int y, m, d;
    char extra;
    if (day == "today") {
        t = localTime(today.tm_year, today.tm_mon, today.tm_mday, hour, minute);
    } else if (day == "tomorrow") {
        t = localTime(today.tm_year, today.tm_mon, today.tm_mday + 1, hour, minute);
    } else if (std::sscanf(day.c_str(), "%4d-%2d-%2d%c", &y, &m, &d, &extra) == 3) {
        if (m < 1 || m > 12 || d < 1 || d > 31) return DAY_INVALID;
        t = localTime(y - 1900, m - 1, d, hour, minute);
        struct tm check;
        localtime_r(&t, &check);
        if (check.tm_mday != d) return DAY_INVALID;   // 2026-02-30
    } else {
        const int want = dayIndex(day);
        if (want < 0) return DAY_INVALID;
        // Today if the time is still ahead, else the next such day.
        const int today_idx = (today.tm_wday + 6) % 7;
        int ahead = (want - today_idx + 7) % 7;
        t = localTime(today.tm_year, today.tm_mon, today.tm_mday + ahead, hour, minute);
        if (int64_t(t) <= now) t = localTime(today.tm_year, today.tm_mon, today.tm_mday + ahead + 7, hour, minute);
    }
    if (t == time_t(-1)) return DAY_INVALID;
    return int64_t(t) <= now ? DAY_PAST : int64_t(t);
}

AlarmTone parseTone(const std::string& tone) {
    AlarmTone t;
    if (tone.empty() || tone == "builtin") return t;
    if (tone.compare(0, 8, "builtin:") == 0) {
        t.value = tone.substr(8);
    } else if (tone.compare(0, 5, "file:") == 0) {
        t.kind = AlarmTone::Kind::File;
        t.value = tone.substr(5);
    } else {
        t.kind = AlarmTone::Kind::Song;
        t.value = tone;
    }
    return t;
}

std::string briefingTone(const std::string& alarm_tone, const std::string& briefing_music) {
    const AlarmTone t = parseTone(alarm_tone);
    if (t.kind == AlarmTone::Kind::File && isValidToneFileName(t.value)) return alarm_tone;
    if (isValidToneFileName(briefing_music)) return "file:" + briefing_music;
    return std::string("builtin:") + BRIEFING_SOFT_TONE;
}

bool isValidToneFileName(const std::string& name) {
    if (name.empty() || name.size() > 48 || name[0] == '.' || name[0] == ' ') return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == ' ' || c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot);
    for (char& c : ext) c = char(std::tolower((unsigned char)c));
    return ext == ".ogg" || ext == ".opus" || ext == ".webm";
}

} // namespace Services
