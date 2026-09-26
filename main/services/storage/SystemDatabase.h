#pragma once

#include "generated/SystemDb.generated.h"

namespace Services {

// /sdcard/db/system.ndb: saved state and settings (alarms later).
// Opened once after the SD card mounts; runs the one-time migrations from
// the old text files. Callers check db().isOpen() before use.
ndb::system::SystemDb& systemDb();
bool openSystemDb();

// The "settings" document; defaults if the database isn't open.
ndb::system::Settings loadSettings();
// Writes the fields in `fields` (Settings::F_* bits).
bool saveSettings(const ndb::system::Settings& settings, uint64_t fields);

} // namespace Services
