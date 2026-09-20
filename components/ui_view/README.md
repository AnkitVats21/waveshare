# ui_view

The on-device touchscreen dashboard: LVGL v9 screens driven by live
`EmbeddedSysDb` state, no network dependency.

## Build gating

This entire component is wrapped in `if(CONFIG_DISPLAY_ENABLE)` in
`CMakeLists.txt`. With the display Kconfig option off, it registers as an
empty no-op component — none of the LVGL/screen code is compiled in.
`CONFIG_DISPLAY_ENABLE` defaults to **on**.

## What's here

- **`DashboardScreen`** / **`AssistantScreen`** (`src/ui/`) — the two main
  LVGL views, built from smaller widgets in `src/ui/widgets/`
  (`Card`, `HBar`, `BarStrip`, `EqualizerBars`, `Sparkline`, `StatRow`,
  `IconButton`, `GroupCarousel`, `LedColorSheet`, `MusicPage`,
  `TranscriptList`, `DetailSheet`).
- **`IUiDataSource`** (`include/ui_view/IUiDataSource.h`) — the interface
  screens code against; they never touch `EmbeddedSysDb` or HTTP types
  directly. Two implementations exist:
  - **`SysDbUiDataSource`** (`src/SysDbUiDataSource.cpp`, this component) —
    the real, on-device implementation. Reads `EmbeddedSysDb` snapshots
    directly, no network hop. (The interface header's own comment still
    calls this "future" — it isn't; it's built and wired in.)
  - **`HttpUiDataSource`** (`tools/lvgl_sim`, not in this component) —
    polls the ESP's HTTP API instead, for the desktop LVGL emulator.
- **`UiSnapshot`** (`include/ui_view/UiSnapshot.h`) — the plain-C++ view
  model shared between this component and `tools/lvgl_sim`. Deliberately
  has zero ESP-IDF/FreeRTOS includes so it compiles standalone on a
  desktop toolchain — keep it that way when adding fields.
- **`PlaceholderData.h`** (`src/ui/`) — every hardcoded/fake value shown
  in the UI, collected in one place, each with a comment on what real data
  source it needs: ambient weather condition/temperature text, assistant
  model/voice display strings, a sample assistant transcript. LED color
  read-back is also fake — the swatch just remembers the last value
  applied in-process, no hardware read-back path exists. When wiring a
  real source for any of these, delete the corresponding entry here.

## Dependencies

Requires `lvgl/lvgl` (`^9.2.2`) and `espressif/esp_lvgl_port` (`^2.9.0`),
declared in this component's own `idf_component.yml` — not in the root
`main/idf_component.yml` (whose lvgl/lcd lines are commented-out leftover
cruft; ignore them). Also requires `core_sysdb` and `media_player`.

## Related

`tools/lvgl_sim` is a **separate**, desktop-buildable LVGL emulator used
to iterate on screens without flashing hardware — it shares `UiSnapshot`
and the `IUiDataSource` contract but is not part of this component's
build.
