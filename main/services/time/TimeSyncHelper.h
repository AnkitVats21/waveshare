#pragma once

#include <cstdint>

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
};

} // namespace Services
