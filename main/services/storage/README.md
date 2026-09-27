# main/services/storage

The app's nexus_db databases (engine: `components/nexus_db`, design:
`docs/nexus-db-design.md`). Schemas are in `schema/db/`; the typed classes
in `generated/` are produced by `tools/starc` at build time and committed.

## system.ndb — `SystemDatabase`

`/sdcard/db/system.ndb`, fsync on every write. Collections:

| Collection | Holds |
|---|---|
| `state` | Saved `SystemState` fields (volume, LED, autoplay, caching, …), bound with `sysdb=` in the schema |
| `settings` | Timezone, Gemini model, voice, system prompt, transcript and session options |
| `alerts` | Per-chime settings (enabled, gain, source file) |
| `alarms`, `reminders` | Keyed by a positive integer id (see `services/alarm`) |

Opened once after the card mounts (on an internal-stack task, as nexus_db
needs), running one-time migrations from the old files (`state_sync.txt`,
`settings.txt`, `gemini_config.json` without its key).

**`SysDbSyncReactor`** saves the bound state fields at most
`SAVE_DELAY_MS` (500 ms) after they change and restores them at boot. It
pauses during an OTA, so the update's LED blink isn't saved as the LED
mode.

## recordings.ndb — `RecordingsDatabase`

`/sdcard/db/recordings.ndb`, fsync on every write. One document per file in
`/sdcard/recordings`, keyed by an id (`"1"`, `"2"`, …) that never changes:
file name, start time, length, mode (stereo / processed), sample rate,
channels, size.

- **Boot check** (background task, after the timezone is set because file
  times are local): new files are read with `RecordingProbe` and added,
  entries whose file is gone are dropped, changed sizes are re-read. Mode,
  rate and start time come from the recorder's file name when it has one.
- **The recorder** adds its file when a recording stops.
- **Rename** changes the file name on the card and writes one field (a
  ~50-byte append); **delete** removes the file, then the document. Both
  fail while the file is open, so deleting stops its playback first.
- A file renamed through the generic file API isn't seen until the next
  boot check.

All calls are serialised by one mutex.
