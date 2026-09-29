#pragma once

#include <cstdint>
#include <string>

namespace Services {

/**
 * @brief When an alarm or reminder fires (docs/alarm-design.md, "Shared with alarms").
 *
 * Portable (host-tested): local time comes from mktime/localtime with the
 * process TZ, so repeat days and DST follow the timezone setting.
 */
struct AlarmWhen {
    uint8_t hour = 0;     // 0-23, local
    uint8_t minute = 0;   // 0-59
    uint8_t days = 0;     // bit0 Mon .. bit6 Sun; 0 = any day (one-shot)
    uint32_t at = 0;      // epoch s of a one-shot alarm or timer; 0 = use hour/minute

    static constexpr uint8_t EVERY_DAY = 0x7F;

    // Fires once: a fixed time, or hour:minute with no repeat days.
    bool oneShot() const { return at != 0 || days == 0; }
};

// The first fire time strictly after `after` (epoch s), or 0 if there is none
// (a fixed time already past). A local time skipped by a DST change fires at
// the time mktime normalises it to (02:30 in a spring-forward gap -> 03:30);
// a repeated local time fires once.
int64_t nextFire(const AlarmWhen& when, int64_t after);

// Days bit (bit0 Mon .. bit6 Sun) of a tm_wday (0 Sun .. 6 Sat).
inline uint8_t dayBit(int tm_wday) { return uint8_t(1u << ((tm_wday + 6) % 7)); }

// Repeat days from words (voice tools): "" or "once" = 0, "daily", "weekdays",
// "weekends", or day names ("mon, wednesday and fri", "Mondays").
// False if a word is not understood.
bool parseDays(const std::string& text, uint8_t& days);
// "once", "every day", "weekdays", "weekends" or "Mon, Wed, Fri".
std::string formatDays(uint8_t days);

// The epoch of hour:minute on `day`, local time: "today", "tomorrow", a day
// name (its next occurrence, today if the time is still ahead) or
// "YYYY-MM-DD". Returns DAY_INVALID if `day` is not understood and
// DAY_PAST if the time has already gone.
constexpr int64_t DAY_INVALID = -1;
constexpr int64_t DAY_PAST = -2;
int64_t resolveDay(const std::string& day, int hour, int minute, int64_t now);

// An alarm's tone setting (AlarmDoc::tone):
//   "" or "builtin"    the default built-in pattern
//   "builtin:<name>"   a built-in pattern (AlertPlayer::alarmToneName)
//   "file:<name>"      a file uploaded to ALARM_TONE_DIR (Opus: .ogg, .opus, .webm)
//   anything else      a library song id (CatalogDB), e.g. a YouTube video
// Whatever can't play rings the built-in tone instead.
constexpr const char* ALARM_TONE_DIR = "/sdcard/media/alarm";
struct AlarmTone {
    enum class Kind { Builtin, File, Song };
    Kind kind = Kind::Builtin;
    std::string value;   // pattern name ("" = default), file name, or song id
};
AlarmTone parseTone(const std::string& tone);
// A plain file name for ALARM_TONE_DIR: letters, digits, space, '-', '_',
// '.', not hidden, at most 48 characters, ending in .ogg, .opus or .webm.
bool isValidToneFileName(const std::string& name);

} // namespace Services
