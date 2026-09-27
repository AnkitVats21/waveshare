# AGENTS.md

Working notes for coding agents (and people) changing this firmware. The
README says what the device does; this file says how the code is laid out,
how to build and test it, and the rules that aren't visible in the code.
Component details live in each component's README (see the map below).

## What this is

Firmware for the Waveshare ESP32-S3 audio board (16 MB flash, 8 MB PSRAM,
two mics, speaker, LED ring, optional LCD): a voice assistant (wake word →
Gemini Live), a music player (YouTube audio via Invidious, cached on the SD
card), alarms, timers and reminders, a recorder, and a web dashboard served
from flash. ESP-IDF v6.0.1, C++17.

The dashboard is a separate repository (`waveshare-dashboard`, React/Vite),
built into a bundle and uploaded to the device.

## Code map

| Where | What |
|---|---|
| `main/main.cpp` | Startup order (below) |
| `main/app/` | App logic: `AppController` (keys, modes), `audio/` (`AudioService`, recorder), `input/`, `led/` |
| `main/services/` | `alarm/`, `alerts/` (chimes), `http/` (server, routes, `/api/ws`), `network/` (Wi-Fi, captive portal), `storage/` (system.ndb, recordings.ndb), `time/` |
| `main/hal/` | Board bring-up: codec, I2C, IO expander, keys, LEDs, LCD |
| `components/audio_core/` | Mic capture, speaker mixer, wake word + AFE, alert mixer, `AudioOrchestrator` (who owns the speaker) |
| `components/core_sysdb/` | `EmbeddedSysDb`: in-RAM system state with change bits, generated from `schema/sysdb.star`; `BufferManager` ring buffers; task priorities (`thread_config.h`) |
| `components/media_player/` | `MusicPlaybackService` (queue, commands), `NexusPlayer` (playback), decoders, WebM/Ogg seeking, Invidious client, `CatalogDB` (library) |
| `components/gemini_live/` | `AssistantService` (session state machine), Gemini Live protocol, audio pump, tool handlers (generated from `schema/gemini_skills_schema.json`) |
| `components/nexus_db/` | Append-only document database on the SD card |
| `components/sd_storage/` | SD mount, `File`/`Fs` wrappers, path policy (what the file API may touch) |
| `components/http_server/` | App-agnostic HTTP server, JSON helpers, web bundle from flash |
| `components/credentials/` | Gemini key and Wi-Fi in NVS |
| `components/ui_view/` | LVGL screens fed from a UI snapshot |
| `schema/` | `sysdb.star` (system state), `db/*.star` (nexus_db databases) |
| `tools/` | `starc` (schema compiler), `webbundle` (dashboard bundle), `voice_test` (speak prompts at the device), `lvgl_sim` |
| `host_tests/` | Portable code built natively and tested with pytest |
| `docs/` | Design docs (nexus_db, alarms), `known-issues.md`, `plans/` |

Old include paths (`app/media_player/NexusPlayer.h`, `common/...`) are
one-line forwarding headers. New code includes the component path
(`media_player/NexusPlayer.h`, `core_sysdb/...`).

## Startup order (`app_main`)

1. Log router, default sysdb values, ring buffers in PSRAM (`BufferManager`).
2. Board (codec, I2C, NVS); SD card; credentials imported from the card
   into NVS if present; `system.ndb` opened and saved state loaded.
3. Wi-Fi (station, or the setup access point).
4. Services: audio, orchestrator, alert player and chimes, assistant,
   player, music service, Gemini audio pump, app controller.
5. Time sync (sets the timezone), then `recordings.ndb` checked against the
   card on a background task (file times are local), alarms, the sysdb
   persistence reactor, HTTP server and `/api/ws`.

## Generated code

Generated at build time from schemas; edit the schema, not the output:

- `schema/sysdb.star` → `components/core_sysdb/.../SystemState.generated.h`
  and its codec (starc).
- `schema/db/*.star` → `main/services/storage/generated/*Db.generated.{h,cpp}`
  (starc; the outputs are committed). The dashboard's `src/lib/ndb_schema.js`
  comes from the same schemas (`npm run ndb` there).
- `components/gemini_live/schema/gemini_skills_schema.json` →
  `gemini_skills_generated.{h,cpp}` (the tools Gemini can call).

## Build, flash, test

```bash
. $IDF_PATH/export.sh            # ESP-IDF v6.0.1
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
./host_tests/run_tests.sh        # builds the native module, runs pytest
host_tests/.venv/bin/pytest host_tests/tests/test_ogg_seek.py -q   # one file
```

**The real configuration is the untracked `sdkconfig`.** `sdkconfig.defaults`
lacks PSRAM and other settings, so a clean checkout (or a git worktree)
builds firmware that asserts at boot. `sdkconfig` also holds the Wi-Fi
password and API key: never commit it, never print it; grep single
non-secret keys only.

On a running device (`nexus.local`, or its IP):

| Task | How |
|---|---|
| OTA firmware | `curl -X POST --data-binary @build/waveshare.bin http://<device>/api/ota` (JSON reply, then reboot) |
| Wait for the reboot | poll `/api/system/metrics` until `uptime_sec` is small |
| Logs | `GET /api/logs?since=<seq>`; the buffer is ~80 lines, so boot lines scroll out; the serial monitor has everything |
| Dashboard | in `waveshare-dashboard`: `NEXUS_HOST=<device> npm run deploy` |
| Is it busy? | `/api/assistant/status`, `/api/music/status`, `/api/alarms/status` before an OTA or anything audible |

HTTP routes are in `main/services/http/routes/` (table in
`main/services/http/README.md`). Downloads answer Range requests.

## Rules that aren't obvious from the code

**Memory**
- Internal RAM is scarce (tens of KB free while playing). Big buffers go in
  PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`, `nexus_db::PsramAllocator`).
- Keep the Opus decode task and anything doing flash writes, partition
  mmap or NVS writes on an internal-RAM stack: PSRAM stacks slow decoding
  5-10× (watchdog) and flash operations freeze the cache. The httpd task's
  stack is in PSRAM, so flash work runs on a worker (`FlashUpload`).
- RTC fast RAM is disabled as heap on purpose: DMA from it returned zeros
  and corrupted SD writes.
- No XIP / code in PSRAM. It was measured and rejected.

**Tasks**
- Priorities and cores are in `core_sysdb/thread_config.h`: audio DSP on
  core 1, network and SD I/O on core 0.
- A task that deletes itself (`vTaskDelete(NULL)`) must hold nothing with a
  destructor in scope (`lock_guard`, `std::string`): the destructor never
  runs, which leaked a mutex and hung "next track". Scope such objects in a
  block that ends before the delete.

**Storage**
- FAT file locking is off (`CONFIG_FATFS_FS_LOCK=0`); `sd_storage::File`
  tracks open files instead: a second writer (or a reader against a writer)
  waits then fails, and `Fs::remove` / `Fs::rename` fail while the file is
  open. Always go through `sd_storage`, and stop readers before deleting or
  renaming (deleting a recording stops its playback first).
- nexus_db databases are append-only logs with an in-RAM index; writes are
  cheap appends, a periodic cleanup rewrites the file. See
  `docs/nexus-db-design.md`.
- The music cache (`/sdcard/music`) deletes files under 32 KB as broken
  downloads; never point it at other folders.

**Network and audio**
- YouTube's CDN throttles audio to ~32 KB/s unless the request carries a
  `Range` header; the stream client always sends one.
- Every HTTPS client verifies the server: call `Tls::secure(config)`
  (`media_player/TlsConfig.h`) or attach `esp_crt_bundle_attach` with the
  hostname check on. The build has no unverified fallback
  (`CONFIG_ESP_TLS_INSECURE` off), so a client without it fails to connect.
- Stream URLs are signed, IP-bound and expire (~6 h); they live in RAM only,
  never in a database or a log line (log the host). googlevideo may answer
  with a 302 to a nearby cache; `HttpClientStream` follows it, since
  `esp_http_client` follows redirects only in `perform()`.
- `/api/ws` accepts one client at a time (the dashboard). Test scripts use
  the REST API, not the WebSocket.
- Chimes, playback and recording start/stop are audible: check with the
  person before running them.

**Serial port**
- One owner only: two programs opening `/dev/ttyACM0` reset-loop the chip.
  If a monitor is running (e.g. in tmux), read its output instead of
  opening the port.

## Security

This repository is public.

- No keys, passwords, tokens, IPs or personal data in code, docs, commits or
  issues.
- The Gemini key and Wi-Fi live in NVS. At boot, `wifi_config.json` on the
  card is imported and deleted, and the key is imported from
  `gemini_config.json` and removed from it (its other settings stay). The
  file API refuses both files (`FileRoutes.cpp`). Never download or print
  them (or `settings.txt` on older cards); read key names only.
- Firmware binaries contain the compiled-in key: `*.bin` stays in
  `.gitignore`.

## Working style

- Isolate and measure a component before fixing integration symptoms.
- Explain the plan before larger changes; commit each working step
  separately, with what was measured on the device in the message
  (`feat(scope): … (step N.x)`, body explains why).
- Portable logic (parsers, schedulers, seek maths) goes in plain C++ with
  host tests; the ESP-specific part stays thin.
- Known bugs and limitations: `docs/known-issues.md`. Update it when you
  find or fix one.
