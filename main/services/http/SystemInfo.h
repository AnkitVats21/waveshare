#pragma once

#include <string>

/**
 * @brief Device facts shared by the REST routes and the /api/ws state push.
 */
namespace SystemInfo {

// Realtime per-core CPU load (cached, refreshed at most every 250 ms).
// Always 0 unless CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS is set.
void getCpuUsage(int& cpu0_pct, int& cpu1_pct);

// Human-readable esp_reset_reason().
const char* resetReason();

// Station IPv4 address, or empty if not connected.
std::string staIp();

} // namespace SystemInfo
