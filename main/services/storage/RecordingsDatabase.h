#pragma once

#include "generated/RecordingsDb.generated.h"

#include <cstdint>
#include <string>

namespace Services {

// /sdcard/db/recordings.ndb: one document per file in /sdcard/recordings,
// keyed by an id that never changes. Callers check db().isOpen() before use.
ndb::recordings::RecordingsDb& recordingsDb();

// Opens the database and checks it against the directory on a background
// task: files not listed are read and added, entries whose file is gone are
// dropped. Call after the timezone is set: file times are read as local time.
void openRecordingsDbAsync();

enum RecordingMode : uint8_t { REC_MODE_UNKNOWN = 0, REC_MODE_STEREO = 1, REC_MODE_PROCESSED = 2 };

// Adds the finished recording at `path` (under /sdcard/recordings), reading
// its length from the file. `started` is epoch seconds, 0 if unknown;
// `sample_rate` 0 takes the rate from the file. False if the database isn't
// open (the next boot's check adds the file) or the write failed.
bool addRecording(const char* path, uint32_t started, RecordingMode mode, uint32_t sample_rate);

enum class RecordingResult { Ok, Unavailable, NotFound, BadName, Taken, Failed };

// Renames the recording's file and rewrites its `file` field; the id stays.
// `name` may omit the extension; the file keeps its own. Letters, digits,
// space and - _ . ( ) only, at most MAX_RECORDING_NAME characters before the
// extension. Failed covers a file that is open (being played).
constexpr size_t MAX_RECORDING_NAME = 60;
RecordingResult renameRecording(int id, const std::string& name, std::string& new_file);
// Deletes the file and its document.
RecordingResult deleteRecording(int id);
// The recording's document and the full path of its file.
RecordingResult findRecording(int id, ndb::recordings::RecordingDoc& doc, std::string& path);

} // namespace Services
