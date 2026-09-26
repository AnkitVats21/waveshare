#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace Services {

/**
 * @brief Keeps the clock set: SNTP with retries, or a time set by hand.
 *
 * Alarms and reminders need the clock, so a failed sync is retried with
 * backoff (30 s up to 15 min) while Wi-Fi is up, rotating through NTP servers
 * (the lwIP build takes one server at a time). Once synced it re-syncs daily.
 * /api/time can trigger a sync now or set the time directly.
 */
class TimeSyncHelper {
public:
    enum class Source : uint8_t { None, Ntp, Manual };

    struct Status {
        bool valid = false;          // clock is set (NTP or manual)
        Source source = Source::None;
        int64_t last_sync = 0;       // epoch of the last successful NTP sync
        std::string server;          // server of the last NTP attempt
        uint32_t failures = 0;       // consecutive failed attempts
        uint32_t next_attempt_s = 0; // seconds until the next attempt, 0 if none due
        bool syncing = false;
    };

    static TimeSyncHelper& instance();

    // Applies the saved timezone and starts the sync task (idle until Wi-Fi).
    void begin();
    // Wi-Fi came up: sync now if the clock is not set.
    void onWifiConnected();
    // Try now (resets the backoff).
    void requestSync();
    // Sets the clock by hand (epoch seconds, UTC). False if out of range.
    bool setTime(int64_t epoch_s);
    Status status();

    // Sets the process timezone (POSIX TZ string, e.g. "IST-5:30").
    static void applyTimezone(const std::string& tz);
    // Clock set at all (year after 2020)?
    static bool clockValid();

private:
    TimeSyncHelper() = default;
    static void taskEntry(void* arg);
    void run();
    bool attempt(const char* server);

    std::mutex m_mutex;
    Status m_status;
    TaskHandle_t m_task = nullptr;
    volatile bool m_wifi = false;
    volatile bool m_sync_now = false;
    int64_t m_next_attempt_us = 0;   // esp_timer time; 0 = none scheduled
    uint32_t m_server_index = 0;
};

} // namespace Services
