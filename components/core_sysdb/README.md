# core_sysdb

The state-management foundation everything else in this firmware builds
on: `EmbeddedSysDb`, the generated `SystemState` schema, and the reactive
task base class (`ReactorTask`).

## What's here

- **`EmbeddedSysDb`** (`include/core_sysdb/EmbeddedSysDb.h`, `src/EmbeddedSysDb.cpp`)
  — a singleton holding one `SystemState` POD struct as the single source
  of truth. All writes go through `EmbeddedSysDb::getInstance().mutate(fn)`,
  which takes a write lock, runs `fn`, diffs the before/after state into a
  per-field change mask, and notifies any `ReactorTask` whose `interest`
  bitmask overlaps the change.
- **`SystemState.generated.h`** — **generated, do not hand-edit**. Compiled
  from `schema/sysdb.star` (two directories up) by `tools/starc/starc.py`,
  wired in as a CMake custom command (`add_custom_command` in this
  component's `CMakeLists.txt`) that reruns automatically whenever
  `schema/sysdb.star` changes and regenerates both
  `SystemState.generated.h` and `src/SysDbCodec.generated.cpp`. You never
  need to run `starc` by hand.
- **`SysDbCodec` / `SysDbTypes.h`** — the generated `diffState()` that
  computes the `(COMP::X | BIT_X::FIELD)` change mask for `mutate()`, and
  the shared schema types (`ComponentId`, `FieldAccess`, `MediaPendingCommand`).
- **`ReactorTask`** (`ReactorTask.h`/`.cpp`) — base class for any service
  that needs to react to state changes without polling. Subclass it,
  declare an `interest` component bitmask, override `onStateChanged()`
  (must return fast — no blocking I/O) and `run()` (does the real work).
  Spawns and pins its own FreeRTOS task, PSRAM-backed stack by default.
- **`BufferManager`** — statically allocated PSRAM ring buffers shared
  across audio/network paths.
- **`LogRouter` / `AsyncNetLogger`** — colorized console logging plus an
  async network log sink.
- **`common/` vs `core_sysdb/` header split**: some headers exist under
  both `include/common/<name>.h` and `include/core_sysdb/<name>.h`. The
  `common/` copies are thin forwarding headers kept for include-path
  compatibility with older code; the `core_sysdb/` copies are canonical —
  prefer including `core_sysdb/...` in new code.

## Schema model

`schema/sysdb.star` currently defines 8 components (`System`, `Audio`,
`Pipeline`, `Assistant`, `Led`, `Alarm`, `Bluetooth`, `Media`), each with
its own bit in a 32-bit `ComponentMask` and its own field set. Component
ids aren't contiguous (`id=5` is retired, formerly `Mqtt`) — `starc`
doesn't require contiguity, so don't assume a gap means a bug.

## Mutation example

```cpp
EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
    s.audio.speaker_volume = 90;
    s.audio.autoplay_enabled = true;
});
```

Never write `SystemState` fields outside a `mutate()` call — direct writes
skip change detection and reactor notification, silently desyncing the UI
(both the on-device LVGL dashboard in `ui_view` and the web dashboard fed
by `/api/ws`) from real device state.
