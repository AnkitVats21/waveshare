#pragma once

#include <cstdint>
#include <string>

namespace Services {

class TimeSyncHelper {
public:
    /**
     * @brief Starts the SNTP service, waits for time synchronization,
     *        sets the timezone, and stops the SNTP service to free memory.
     * @param timeout_ms  Max time to wait for synchronization.
     * @return true if time was successfully synchronized.
     */
    static bool synchronizeTimeAndCleanup(uint32_t timeout_ms = 15000);

    // Sets the process timezone (POSIX TZ string, e.g. "IST-5:30").
    static void applyTimezone(const std::string& tz);
};

} // namespace Services
