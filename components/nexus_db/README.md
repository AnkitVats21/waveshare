# nexus_db

A small document database for the SD card: an append-only log of records
with an in-RAM index. Used by `system.ndb` (state, settings, alerts,
alarms, reminders) and `recordings.ndb`; `music.ndb` is next. The file
format, recovery rules and the reasons behind them are in
`docs/nexus-db-design.md`.

## Model

- A **database** is one file (`/sdcard/db/<name>.ndb`) holding several
  **collections**; a collection holds **documents** keyed by a string.
- A document is a list of tagged fields (tag, length, value). Readers skip
  unknown tags and fall back to defaults for missing ones, so fields can be
  added or retired without migrating the file.
- Writes append a record: `put` (whole document), `merge` (some fields;
  cached collections only), `remove`. The index maps each key to its latest
  record. **Cleanup** rewrites the file with live documents only, when dead
  bytes exceed live bytes.
- Durability per database: `EveryCommit` (fsync after each write) or
  `Batched` (fsync at most `FLUSH_INTERVAL_MS` after a write; the owner
  calls `flushIfDue()`).
- Open recovers from a torn last record, a corrupt region and an
  interrupted cleanup.

## Schemas and generated code

Databases are declared in `schema/db/*.star` and compiled by
`tools/starc` into typed classes (`main/services/storage/generated/`):
a `Doc` struct per collection with `F_*` field bits, `encode`/`decode`, and
a database class with one `Collection<Doc>` per collection. The same
schema also produces the dashboard's JavaScript reader schema.

```cpp
auto& db = Services::recordingsDb();
ndb::recordings::RecordingDoc doc;
if (db.recordings().get("7", doc)) { ... }
ndb::recordings::RecordingDoc update;
update.file = "kitchen.opus";
db.recordings().merge("7", update, ndb::recordings::RecordingDoc::F_FILE);
```

`Binding.h` copies fields bound to `SystemState` (`sysdb=` in the schema),
which is how saved state is restored at boot.

## Files

| File | What |
|---|---|
| `Database.h` | Open, put/merge/get/remove/forEach, flush, cleanup; thread safe (one mutex; `forEach` callbacks must not call back in) |
| `Collection.h` | Typed view over one collection |
| `Fields.h` | Field writer and reader |
| `Io.h` | File operations; the device uses `sd_storage`, host tests use POSIX files with injected faults |
| `PsramAllocator.h` | Keeps the index and cached documents out of internal RAM |
| `StressTest.h` | Power-loss test (`CONFIG_NEXUS_DB_STRESS_TEST`): appends, then `esp_restart` at random points |

## Tests

`host_tests/tests/test_nexus_db.py`: format, recovery (torn tail,
corruption, cleanup interrupted at each step), merges, cleanup. The
on-device stress test runs rounds while `/sdcard/db/stress.run` exists.

The dashboard reads a database by downloading it (`GET /api/db/<name>`)
and parsing it with `tools/starc/js/ndb.js`; `ndb_dump.mjs` prints one on
the command line.
