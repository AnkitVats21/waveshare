#pragma once

#include <cstdint>

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

} // namespace Services
