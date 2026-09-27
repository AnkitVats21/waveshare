# Nexus: firmware for the Waveshare ESP32-S3 audio board

[![Build Firmware](https://github.com/AnkitVats21/waveshare/actions/workflows/build.yml/badge.svg)](https://github.com/AnkitVats21/waveshare/actions/workflows/build.yml)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0.1-blue.svg)](https://docs.espressif.com/projects/esp-idf/)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17-green.svg)](https://en.cppreference.com/)
[![License](https://img.shields.io/badge/License-MIT-purple.svg)](LICENSE)

C++17 firmware that turns the Waveshare ESP32-S3 audio board into a
voice-controlled music box: say the wake word and talk to Google's Gemini
Live, play music from YouTube (via Invidious) with songs saved to the SD
card, set alarms, timers and reminders, record audio, and control it all
from a web dashboard the device serves itself at `http://nexus.local`.

## Features

- **Voice assistant** — on-device wake word (ESP-SR, with echo
  cancellation) opens a live, two-way audio session with Gemini Live over a
  direct WebSocket. Gemini can call 25 device tools: play music, control
  playback and volume, set alarms, timers and reminders, set the LEDs, keep
  notes.
- **Music** — search and stream YouTube audio through Invidious, with a
  queue, autoplay of related songs, and exact seeking. With caching on,
  songs are saved to the SD card while they play and replay from the card.
- **Alarms, timers, reminders** — an alarm takes over the speaker (pausing
  music and ending an assistant session), plays a library song or a built-in
  tone, snoozes, and restores the music afterwards.
- **Recorder** — records both mics (stereo) or the cleaned-up voice signal
  as Ogg Opus; recordings can be played in the browser or on the device,
  renamed and downloaded.
- **Web dashboard** — player, library, queue, assistant, alarms,
  recordings, files and settings, served from flash with over-the-air
  updates for both the dashboard and the firmware
  ([waveshare-dashboard](https://github.com/AnkitVats21/waveshare-dashboard)).
- **On-device screen** (optional) — LVGL touchscreen UI.
- **Robust storage** — settings, state, alarms and the recordings list live
  in small append-only databases on the card that survive power cuts.

## Hardware

| Part | |
|---|---|
| MCU | ESP32-S3, dual-core 240 MHz, Wi-Fi, 16 MB flash, 8 MB octal PSRAM |
| Audio | ES7210 mic ADC (two MEMS mics + echo reference), ES8311 codec, class-D speaker amplifier |
| Storage | microSD (SDMMC, FAT32) |
| Controls | Five keys through a TCA95xx I2C expander, WS2812 LED ring |
| Display (optional) | 320×240 ILI9341 SPI screen with XPT2046 touch (`CONFIG_DISPLAY_ENABLE`) |

## Getting started

1. **Toolchain:** ESP-IDF v6.0.1.
   ```bash
   . $IDF_PATH/export.sh
   idf.py build
   idf.py -p /dev/ttyACM0 flash monitor
   ```
   `idf.py menuconfig` → *Waveshare Audio Development Board Config* holds the
   Wi-Fi, wake word, SD card and display options. PSRAM and the partition
   table must be enabled as in `sdkconfig.defaults` plus your board's needs.
2. **SD card:** a FAT32 microSD card in the slot.
3. **Credentials** (stored in NVS, never on the card), any of:
   - put `wifi_config.json` (`{"ssid": "...", "password": "..."}`) and
     `gemini_config.json` (`{"api_key": "..."}`) on the card and boot: they
     are imported and removed from the card;
   - with no Wi-Fi configured (or after five failed connection attempts),
     the device opens a setup access point: join it and use the portal;
   - `POST /api/config/gemini` and `POST /api/wifi/configure` from the
     dashboard's settings.
4. **Dashboard:** open `http://nexus.local` (or the device's IP). If no
   dashboard bundle is installed yet, a built-in page is served at `/` and
   always at `/recovery`. Build and upload the dashboard from its repository
   with `npm run deploy`.
5. **Updates:** later firmware goes over the air:
   `curl -X POST --data-binary @build/waveshare.bin http://nexus.local/api/ota`.

## How it fits together

```
 keys / wake word / dashboard (/api/ws, REST) / Gemini tool calls
                         │
              AppController, HTTP routes
                         │
   ┌──────────────┬──────┴───────┬───────────────┬──────────────┐
 Assistant      Music          Alarms         Recorder       Storage
 (gemini_live)  (media_player) (services/     (app/audio/    (nexus_db,
                               alarm)          recording)     sd_storage)
   └──────────────┴──────┬───────┴───────────────┘
              AudioOrchestrator (who owns the speaker)
                         │
          audio_core: mic capture, AFE + wake word, mixer, I2S
```

All live state sits in one in-RAM store, `EmbeddedSysDb`: services change
it through `mutate()`, and tasks that care (the dashboard channel, LEDs,
audio, persistence) are woken by per-field change bits. What must survive a
restart is saved to `system.ndb` on the card.

| Part | README |
|---|---|
| Audio I/O, wake word, mixer, orchestrator | [components/audio_core](components/audio_core/README.md) |
| System state, ring buffers, task layout | [components/core_sysdb](components/core_sysdb/README.md) |
| Music player, seeking, library | [components/media_player](components/media_player/README.md) |
| Voice assistant and tools | [components/gemini_live](components/gemini_live/README.md) |
| Databases on the card | [components/nexus_db](components/nexus_db/README.md) |
| SD card access | [components/sd_storage](components/sd_storage/README.md) |
| Credentials | [components/credentials](components/credentials/README.md) |
| HTTP server core | [components/http_server](components/http_server/README.md) |
| Screen UI | [components/ui_view](components/ui_view/README.md) |
| App logic, keys, recorder | [main/app](main/app/README.md) |
| Alarms, timers, reminders | [main/services/alarm](main/services/alarm/README.md) |
| REST API and `/api/ws` | [main/services/http](main/services/http/README.md) |
| system.ndb, recordings.ndb | [main/services/storage](main/services/storage/README.md) |
| Board bring-up | [main/hal](main/hal/README.md) |
| Schema compiler, bundler, voice tests | [tools](tools/README.md) |
| Host (PC) tests | [host_tests](host_tests/README.md) |

## Documentation

- [AGENTS.md](AGENTS.md) — code map, build and test, and the rules that
  aren't visible in the code; start here before changing anything.
- [docs/known-issues.md](docs/known-issues.md) — current bugs and
  limitations.
- [docs/nexus-db-design.md](docs/nexus-db-design.md) — the database format
  and the storage plan.
- [docs/alarm-design.md](docs/alarm-design.md) — alarms, timers and
  reminders.
- [docs/plans/](docs/plans/) — proposals not started yet.

## Related repositories

- [waveshare-dashboard](https://github.com/AnkitVats21/waveshare-dashboard) —
  the web dashboard (React/Vite).
- [invidious-daemon](https://github.com/AnkitVats21/invidious-daemon) — a
  self-hosted Invidious-backed audio resolver, deployed separately.
- [starhub](https://github.com/AnkitVats21/starhub) — retired; the device now
  serves its own control channel.

## License

MIT, see [LICENSE](LICENSE).
