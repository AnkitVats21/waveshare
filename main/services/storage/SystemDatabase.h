#pragma once

#include "generated/SystemDb.generated.h"

namespace Services {

// /sdcard/db/system.ndb: saved state (and later settings and alarms).
// Opened once after the SD card mounts; runs the one-time migrations from
// the old text files. Callers check db().isOpen() before use.
ndb::system::SystemDb& systemDb();
bool openSystemDb();

} // namespace Services
