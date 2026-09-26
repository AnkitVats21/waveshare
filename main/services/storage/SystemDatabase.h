#pragma once

#include "generated/SystemDb.generated.h"

#include <utility>
#include <vector>

namespace Services {

// /sdcard/db/system.ndb: saved state, settings, alert chimes, alarms, reminders.
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

// Alarms and reminders, keyed by a positive integer id. Lists are sorted by
// id and empty if the database isn't open.
std::vector<std::pair<int, ndb::system::AlarmDoc>> listAlarms();
bool loadAlarm(int id, ndb::system::AlarmDoc& doc);
bool saveAlarm(int id, const ndb::system::AlarmDoc& doc);
bool mergeAlarm(int id, const ndb::system::AlarmDoc& doc, uint64_t fields);
bool removeAlarm(int id);
int nextAlarmId();   // one past the highest id in use

std::vector<std::pair<int, ndb::system::ReminderDoc>> listReminders();
bool loadReminder(int id, ndb::system::ReminderDoc& doc);
bool saveReminder(int id, const ndb::system::ReminderDoc& doc);
bool mergeReminder(int id, const ndb::system::ReminderDoc& doc, uint64_t fields);
bool removeReminder(int id);
int nextReminderId();

} // namespace Services
