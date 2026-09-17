# lvgl_sim

Desktop LVGL emulator for the Waveshare ESP32-S3 touchscreen UI. Renders the
same 240x320 screens in an SDL2 window, driven by polling the ESP's real
HTTP API over Wi-Fi — no flashing required to iterate on UI.

Standalone CMake project, not part of the ESP-IDF `idf.py build`.

## Architecture

- `components/ui_view/` (repo-level, shared) — plain-C++ `UiSnapshot` view
  model and `IUiDataSource` interface. No ESP-IDF/FreeRTOS dependency.
- `src/HttpUiDataSource` — the only emulator-specific piece: polls
  `/api/music/status` and `/api/system/delta` on a background thread and
  fills a `UiSnapshot` behind a mutex (same single-writer/multi-reader shape
  as the firmware's `EmbeddedSysDb`).
- `src/ui/NowPlayingScreen`, `src/ui/DashboardScreen` — LVGL widgets that
  only read `UiSnapshot` and call `IUiDataSource` for actions. These are
  meant to be reusable unchanged once the real display exists on-device:
  swap `HttpUiDataSource` for a future `SysDbUiDataSource` (backed by
  `EmbeddedSysDb::getInstance().snapshot()`) and nothing else changes.
- `main.cpp` — SDL2 window/renderer as the LVGL v9 display + mouse-as-touch
  input, auto-switches between the two screens based on playback state.

## Build

```bash
cmake -S tools/lvgl_sim -B tools/lvgl_sim/build
cmake --build tools/lvgl_sim/build -j
```

First configure fetches LVGL v9.2.2 via `FetchContent` (requires network).

## Run

```bash
./tools/lvgl_sim/build/lvgl_sim --host 192.168.x.x
# or: ESP_HOST=192.168.x.x ./tools/lvgl_sim/build/lvgl_sim
```

Find the device's IP from its own web dashboard or `GET /api/system/init`.

Verified against a real board at `192.168.1.14` (2026-09-17): builds clean,
connects, and correctly picked up a `PAUSED` track from `/api/music/status`.
