#pragma once

#include "generated/SystemDb.generated.h"

namespace Services {

// /sdcard/db/system.ndb: saved state, settings and alert chimes (alarms later).
// Opened once after the SD card mounts; runs the one-time migrations from
// the old text files. Callers check db().isOpen() before use.
ndb::system::SystemDb& systemDb();
bool openSystemDb();

// The "settings" document; defaults if the database isn't open.
ndb::system::Settings loadSettings();
// Writes the fields in `fields` (Settings::F_* bits).
bool saveSettings(const ndb::system::Settings& settings, uint64_t fields);

// The alert's document; false (and `config` left at defaults) if it has none.
bool loadAlertConfig(const char* name, ndb::system::AlertConfig& config);
// Writes the fields in `fields` (AlertConfig::F_* bits).
bool saveAlertConfig(const char* name, const ndb::system::AlertConfig& config, uint64_t fields);
// Deletes the document, returning the alert to its default.
bool resetAlertConfig(const char* name);

} // namespace Services
