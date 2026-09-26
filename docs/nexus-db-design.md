# nexus_db: on-device databases

Status: steps 1–3 done (alarms deferred to the alarm redesign). 2026-09-26.

## Goals

- One storage engine (`components/nexus_db`) shared by every part of the
  firmware that keeps records: saved state, settings, alarms, the music
  catalogue, recordings.
- One database file per area, so each area has its own lock, cleanup and
  failure, and can be wiped alone.
- Safe on power loss: a cut can lose at most the record being written, never
  earlier records.
- Adding a field is a schema edit; no hand-written parsing or migration.
- The dashboard downloads the raw file and decodes it; the device never builds
  JSON for these lists.
- Credentials never go in these files. They live in NVS.

Non-goals: SQL, secondary indexes, transactions across records. At our scale
(at most ~1000 tracks, tens of alarms and recordings) scanning an in-RAM index
is fast enough.

## Files

| Database | Path | Collections | Rebuildable? | Flush |
|---|---|---|---|---|
| system | `/sdcard/db/system.ndb` | `state`, `settings`, `alerts` (`alarms` with the alarm redesign) | no | every commit |
| music | `/sdcard/db/music.ndb` | `tracks` | yes (scan `/sdcard/music`) | batched, ≤ 2 s |
| recordings | `/sdcard/db/recordings.ndb` | `recordings` | yes (scan `/sdcard/recordings`) | batched, ≤ 2 s |

Later candidates: `notes.ndb` for Gemini notes and memory.

Credentials (NVS, namespace `creds` and the existing `wifi_store`):
`gemini_api_key`, Wi-Fi SSID and password. The Gemini model and voice are
ordinary settings and go in `system.ndb`.

## Engine model

Each file is an append-only log of records. A record puts, merges or deletes
one document, identified by `(collection, key)`. The log is the database: there
is no separate data file and WAL.

- **Put** appends the whole document. **Merge** appends only the changed
  fields. **Delete** appends a delete marker.
- **Index**: at open, the engine reads the log once and builds a hash index in
  PSRAM: `(collection, key) → offset, length`. The latest record for a key wins.
- **RAM-cached collections** (schema option `cache`): the decoded document is
  also kept in PSRAM, so listing and filtering never touch the card. All
  collections planned so far are cached; the option exists for future
  collections with large documents.
- **Merge is allowed only on cached collections.** Their current document is in
  RAM, so a merge applies to it directly, and a lookup never has to combine a
  chain of records from the card. Uncached collections use put and delete only.
- **Get** on a cached collection is a RAM lookup; otherwise one `pread`.
- **Queries**: `forEach(collection, fn)` over the index or cache; callers filter.

### Concurrency

One mutex per database. Writers append under it; reads of cached collections
take it briefly. Writers are background tasks (sync reactor, catalogue, alarm
service), never the audio path.

### Durability

- `system`: `fsync` after every commit. These writes are rare (debounced state
  changes, user edits).
- `music`, `recordings`: `fsync` at most every 2 s, and at close. Losing the
  last records on power loss is acceptable because both can be rebuilt by
  scanning the folders.

## File format

All integers little-endian. Records are 4-byte aligned.

### Header (32 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `"NXDB"` |
| 4 | 2 | format version (1) |
| 6 | 2 | header length (32) |
| 8 | 4 | schema hash (of the schema that last wrote the header; a file opened with another schema is cleaned up once, which rewrites it) |
| 12 | 4 | generation (incremented by each cleanup) |
| 16 | 12 | database name, NUL-padded (`"system"`, `"music"`, ...) |
| 28 | 4 | CRC-32 of bytes 0–27 |

### Record

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | sync marker `0x5AA5` |
| 2 | 2 | record length, including this header and padding |
| 4 | 4 | CRC-32 of bytes 8 .. end |
| 8 | 4 | sequence number (monotonic within the file) |
| 12 | 1 | collection id |
| 13 | 1 | op: 1 put, 2 merge, 3 delete, 4 seal |
| 14 | 1 | key length (≤ 64) |
| 15 | 1 | flags (reserved, 0) |
| 16 | n | key bytes (UTF-8, no NUL) |
| .. | .. | value: a sequence of fields (empty for delete) |
| .. | 0–3 | zero padding to a multiple of 4 |

Record length is 16 bits, so a record is at most 64 KB. Planned records are
a few hundred bytes.

### Field

| Size | Field |
|---|---|
| 2 | tag (from the schema, 1–65535) |
| 2 | length in bytes |
| n | value |

Value encodings by schema type: `bool`/`u8`/`i8` 1 byte; `u16`/`i16` 2;
`u32`/`i32`/`f32` 4; `u64`/`i64` 8; `string` UTF-8 without NUL; `bytes` raw;
`rgb` 3 bytes. Readers skip unknown tags and fields whose length does not match
the schema type; missing tags take the schema default. So fields can be added,
removed or change type without migrating old files.

### Reading at open

1. Check the header. If it is bad, go to recovery.
2. Read records in order. For each: check the sync marker, the length and the
   CRC, then apply it to the index or cache.
3. A record that is short or fails its CRC at the end of the file is a cut-off
   write: truncate the file there and continue.
4. A bad record with valid records after it is card corruption, not a power
   cut. Scan forward for the next sync marker whose record passes its CRC, log
   the number of bytes skipped, and schedule a cleanup so the damaged region is
   dropped. Only the damaged records are lost.

## Cleanup (compaction)

Trigger: dead bytes > live bytes and file > 64 KB, checked after commits and at
open.

1. Write `<name>.ndb.tmp`: header with generation + 1, then one put per live
   document (merges folded in), then a **seal** record whose value holds the
   record count.
2. `fsync`, then replace the old file. `sd_storage::Fs` already does this
   (unlink old, rename new) and already recovers a leftover `.tmp` when the
   target is missing, which covers a power cut between the unlink and the
   rename.
3. A `.tmp` without a valid seal is an interrupted cleanup and is deleted.

Cleanup of `music.ndb` with ~1000 tracks writes roughly 300 KB and takes well
under a second.

## Serving the raw file to the dashboard

`GET /api/db/<name>` (name is one of `system`, `music`, `recordings`).

- Opens the file read-only through `sd_storage::File`. While it is open,
  `sd_storage` blocks the replace step of cleanup, so the file cannot be swapped
  mid-download. Appends continue.
- The response length is fixed when the request starts; records appended later
  are not sent. A record half-written at that moment fails its CRC on the
  dashboard and is dropped, exactly as at boot.
- The generic file API refuses writes and deletes under `/sdcard/db/`. Reads
  are allowed; these files hold no secrets.

The dashboard runs the same reading steps as the device (header, records, CRC,
latest-wins, merge, delete markers) in `src/lib/ndb.js`. The field layout comes
from a generated `src/lib/ndb_schema.js`, so device and dashboard read the same
schema.

## Schema

One schema file per database under `schema/db/`, compiled by an extension of
`tools/starc`. Tags are explicit and never reused, like protobuf field numbers.

```
database music path="/sdcard/db/music.ndb" flush=batched {
    // Key: the YouTube video id (e.g. "ApCL2GomTD4"), stored in the record
    // header, not as a field. Thumbnails are /sdcard/music/thumbs/<key>.jpg.
    collection tracks id=1 key=string cache {
        field title:          string   tag=1
        field artist:         string   tag=2
        field album:          string   tag=3
        field duration_ms:    u32      tag=4
        field file_size:      u32      tag=5   // 0 = not cached
        field codec:          u8       tag=6   // 0 WebM/Opus, 1 WAV, 2 Ogg/Opus
        field cached_at:      u32      tag=7
        field last_played_at: u32      tag=8
        field play_count:     u32      tag=9
        field flags:          u8       tag=10  // pinned, has thumbnail
    }
}
```

The compiler generates:

- a C++ struct per collection, with `encode(Writer&)` and `decode(Reader&)`;
- a typed wrapper, e.g. `MusicDb::tracks().get(video_id)`, `.put(track)`,
  `.merge(id, {play_count})`;
- `ndb_schema.js` for the dashboard;
- the schema hash stored in the header.

Strings have no fixed size on disk (titles are no longer cut at 64 bytes). The
C++ struct uses `std::string` for them; documents live in PSRAM.

### The `state` collection

`state` in `system.ndb` is one document, key `"state"`, holding the
`SystemState` fields that survive a reboot. They are listed in
`schema/db/system.star` like any other collection, each with `sysdb=` naming
the live field:

```
collection state id=1 key=string cache doc=SavedState {
    field speaker_volume: i32 = 80 tag=1 sysdb=audio.speaker_volume
    field led_mode:       u8       tag=4 sysdb=led.mode
}
```

So the whole file is described by one schema, which is also where the
dashboard's JS schema comes from. starc generates `SavedState::fromSysdb()`
and `toSysdb()`. Defaults must match `sysdb.star`, because a field missing from
the file is applied with its default.

Saved fields in version 1: `audio.speaker_volume`, `audio.mic_gain_db`,
`led.color`, `led.mode`, `media.autoplay_enabled`, `media.cache_downloads`.
`pipeline.mode` is not saved: `AssistantService` derives it from the session
state.

`SysDbSyncReactor` saves 500 ms after the first change of an AUDIO, LED or
MEDIA field, not after the last one: media fields change continuously during
playback. It puts the whole document; the engine writes nothing when it is
unchanged. Saving pauses during a firmware OTA, whose green blink is followed
by a reboot and must not become the saved LED mode.

### The `settings` collection

One document, key `"settings"`: `timezone` (POSIX TZ, default `"UTC"`),
`gemini_model`, `gemini_voice`, `gemini_system_prompt` (empty = firmware
default). `/api/config/settings` reads and writes `{"timezone"}` and applies
it immediately; `/api/config/gemini` keeps its shape, with the API key still
write-only in NVS. `gemini_live` gets these through a callback set by `main`
(`GeminiProtocol::setSettingsSource`), so it does not depend on the database.

Dropping a `gemini_config.json` on the card still works on a fresh device:
at boot the key goes to NVS first, then the other fields to `settings`, and
the file is deleted. A file still holding a key is never deleted.

### The `alerts` collection

One document per alert chime, keyed by its name (`wake_confirm`,
`ready_to_speak`, `session_end`, `error`, `offline`): `enabled` (default
true), `source` and `gain_db` (-24..+6, default 0). `source` is `""` for the
default, `"builtin"` for the synthesized tone, or a plain file name in
`/sdcard/media/alert/`. An alert with no document is the default: its
`<name>.ogg` if the card has one, else the built-in tone. Resetting an alert
deletes its document, so there is nothing to migrate. `AlertLibrary` reads the
document when it decodes the alert at boot or on a change; a file that is
missing or fails to decode falls back to the built-in tone.

`/api/alerts` (see `AlertRoutes.cpp`) lists the alerts, the files in the
folder and the limits; `POST /api/alerts/<name>` changes `enabled`, `gain_db`
or `source`; `/upload` stores a file only after it decodes; `/play` previews;
`/reset` deletes the document.

## Seeking

No seek table is stored. The player finds byte positions from the file:

- **WebM**: YouTube audio files (itag 251) carry a Cues element, WebM's own
  seek index, near the start of the file, before the first audio cluster.
  Checked on 6 tracks (170 s to 2 h): Cues start at byte 259 or 266, audio at
  byte 500–1045 (the 2 h track's Cues are longer). One cue per cluster, about
  every 10 s. The decoder parses the Cues when it opens a cached file, and
  from the first bytes of a stream, so the whole track is seekable, including
  parts never played.
- **Ogg Opus** (recordings, `.ogg` downloads): no index in the format. Seek by
  binary search on page granule positions: a few small reads per seek.
- **Fallback** (no Cues found): estimate from the average bitrate, as today.

This replaces `CatalogDB::setSeekTable`, `lookupSeekEntry` and
`NexusPlayer::_sessionSeekTable`, which are deleted.

## Migration (one time, at first boot of the new firmware)

| Old file | Goes to | Afterwards |
|---|---|---|
| `/sdcard/state_sync.txt` | `system.ndb` `state` | renamed `.bak` |
| `/sdcard/alarms.json` | `system.ndb` `alarms` (with the alarm redesign) | renamed `.bak` |
| `/sdcard/settings.txt` | `system.timezone` → `settings.timezone` | **deleted** (held unused plaintext Wi-Fi and MQTT credentials) |
| `/sdcard/playback.txt`, `playback_history.txt` | nothing (written by an agent rule, read by no code) | **deleted** |
| `/sdcard/music/catalog.db` (+ `.wal`) | `music.ndb` `tracks` (seek tables dropped) | renamed `.bak` |
| `/sdcard/recordings/*` | `recordings.ndb` (scan) | files unchanged |
| `/sdcard/gemini_config.json` | `api_key` → NVS; `model`, `voice`, `system_prompt` → `settings` | **deleted** once no key is left in it |
| `/sdcard/wifi_config.json` | NVS `wifi_store` | **deleted** after the NVS value reads back |

Each import runs only when the target is empty and the old file exists, so an
interrupted migration simply repeats. The card-file import for Wi-Fi and Gemini
stays as a provisioning path for a fresh device: drop the JSON on the card, and
the next boot moves it into NVS and deletes it.

## Credentials in NVS

- `creds/gemini_api_key`: read by `GeminiProtocol::readConfig`; written by the
  dashboard config POST. The key is never returned by any endpoint (only
  `api_key_set`).
- `wifi_store/ssid`, `wifi_store/password`: already written by `WifiService`;
  NVS becomes the only source.
- NVS encryption is off. An `nvs_keys` partition exists for when it is turned
  on; HMAC-based encryption needs an eFuse key burned permanently, so it waits
  for the planned token server, after which the device holds only short-lived
  tokens.
- `idf.py erase-flash` wipes credentials; `app-flash` and OTA do not.

## Implementation steps

Each step is its own commit and is tested on the device before the next.

1. **NVS credentials + migration.** Gemini key and Wi-Fi from NVS; import and
   delete the JSON files. Test: fresh import, reboot, session config read, key
   never in any response or log.
2. **nexus_db engine + starc extension** (C++ and JS output). Host tests for
   the format (torn tail, mid-file corruption, cleanup interrupted at each
   step); an on-device stress test that appends while pulling power is
   simulated by `esp_restart` at random points.
3. **system.ndb**: state, settings and alert chimes, migrations, `GET /api/db/<name>`. Alarms move with the alarm redesign (own design doc).
4. **Seeking from the file itself** (before music.ndb, so nothing seek-related
   is migrated): see "Seeking" below.
5. **music.ndb**: `CatalogDB` becomes a wrapper; migration from `catalog.db`.
6. **recordings.ndb**: `AudioRecorder` adds a record on stop; scan on first
   open.
7. **Dashboard** (separate repo and commit): `ndb.js` reader, generated
   schema, library / alarms / recordings views on `/api/db/<name>`.

## Decisions

- Recording documents hold name, start time, length, mode and size.
- Thumbnails stay as files in `/sdcard/music/thumbs/<id>.jpg`; the dashboard
  loads them by track id.
- No code for cleaning up leftover download `.tmp` files; they are removed by
  hand.
- Credentials are never stored in a plain file on the card again.
