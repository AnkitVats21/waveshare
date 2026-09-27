# tools/starc

The schema compiler. Two kinds of `.star` schema, one tool:

| Command | Input | Output |
|---|---|---|
| `starc.py <schema> --out-header H --out-codec C` | `schema/sysdb.star` (system state components and fields) | `SystemState.generated.h` and the diff codec (`core_sysdb`) |
| `starc.py db <schemas...> --out-dir D [--js F]` | `schema/db/*.star` (nexus_db databases) | `<Name>Db.generated.{h,cpp}` per database; with `--js`, the dashboard's `ndb_schema.js` |

Both run from CMake during the firmware build (`components/core_sysdb`,
`main/CMakeLists.txt`); you rarely call them by hand. The dashboard
regenerates its schema with `npm run ndb`.

- `starc.py` — the sysdb compiler and the entry point.
- `ndb_gen.py` — the nexus_db generator (`db` subcommand).
- `js/ndb.js` — a nexus_db reader for JavaScript (copied into the
  dashboard); `js/ndb_dump.mjs <file.ndb> <ndb_schema.js>` prints a
  database as JSON.

## Schema rules (nexus_db)

- A field's `tag` is its identity in the file: never reuse or change one;
  retire a removed field's tag with a comment.
- Adding a field needs no migration: old documents read the default.
- `cache` collections keep documents in RAM and support `merge`.
- `sysdb=<component.field>` binds a field to `SystemState` for save and
  restore.
