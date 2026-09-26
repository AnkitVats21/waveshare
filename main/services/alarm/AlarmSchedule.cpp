#include "services/alarm/AlarmSchedule.h"

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

} // namespace Services
