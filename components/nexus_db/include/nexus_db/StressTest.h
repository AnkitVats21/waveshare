#pragma once

namespace nexus_db {

// Test builds (CONFIG_NEXUS_DB_STRESS_TEST): runs one round of the power-loss
// stress test if /sdcard/db/stress.run exists. Doesn't return while rounds
// remain (the round ends in esp_restart). Call right after the SD card is
// mounted, before anything else writes to it.
void runStressTestIfRequested();

} // namespace nexus_db
