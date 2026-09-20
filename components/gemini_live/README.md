# gemini_live

The voice assistant: a direct, bidirectional WebSocket connection to
Google's Gemini Live API, plus the tool/function-calling router that lets
Gemini actually control the device.

## What's here

- **`GeminiProtocol`** — the WebSocket client (`esp_websocket_client` via
  the `WssClient` RAII wrapper) talking directly to
  `wss://generativelanguage.googleapis.com`, no proxy. Loads the API key
  from `/sdcard/gemini_config.json` (`api_key` field only) if present,
  falling back to the `GEMINI_API_KEY` Kconfig value. The Gemini model
  itself (`gemini-3.1-flash-live-preview` at time of writing) is baked
  into the generated setup handshake, not configurable via that JSON file.
- **`GeminiAudioPump`** — Core 1 task pumping `MicCapture` (16kHz PCM)
  uplink audio into the WS connection and Gemini's 24kHz downlink audio
  out to the speaker path (resampled to 32kHz by `audio_core`'s
  `Resampler`).
- **`AssistantService`** — the assistant state machine (idle / listening /
  speaking / tool-executing), triggered by `WakeWordEngine` detections
  from `audio_core`.
- **`DeviceCommandHandler`** / **`MediaCommandHandler`** — the tool-call
  router. Dispatches parsed Gemini function calls to `AppController` (in
  `main/app`) via `IDeviceCommandDelegate`, which is the actual command
  entry point the rest of the firmware exposes.
- **`IVoiceTransport`** — an abstraction the header comments describe as
  supporting both a "Direct Mode" (`GeminiProtocol`) and a "Relayed Mode"
  (`RelayVoiceClient`, via a local hub). **Only `GeminiProtocol` actually
  exists in this codebase** — there is no `RelayVoiceClient` anywhere.
  Treat the relayed mode as aspirational/undocumented-future-work, not a
  real option.

## Tool-calling surface

The tool list is **generated**, not hand-written: `schema/gemini_skills_schema.json`
is compiled by `scripts/generate_gemini_skills.py` into
`gemini_skills_generated.h`/`.cpp` via a CMake custom command (runs
automatically on build, same pattern as `core_sysdb`'s schema compiler).
To add/change a tool, edit the JSON schema, not the generated files.

Current tools (verify against `gemini_skills_generated.cpp`'s
`SETUP_HANDSHAKE_JSON` if this drifts):

| Function | Purpose |
|---|---|
| `play(query)` | Play a track immediately (clears queue) |
| `play_next(query)` | Queue a track to play right after the current one |
| `pause` / `resume` / `stop` | Playback transport |
| `next` / `previous` | Skip within queue/history |
| `volume(level)` | Player-level volume (0-100) |
| `mute` | Toggle mute |
| `autoplay(enabled)` | Toggle autoplay recommendations |
| `set_caching(enabled)` | Toggle SD-card stream caching |
| `set_device_volume(level)` | Physical hardware speaker volume |
| `set_led_strip(r,g,b)` | Solid status LED color |
| `restart_websocket_client` | Reinit the Gemini WS connection |
| `read_file(path)` / `write_file(path, content)` | SD card text file I/O (`/sdcard/...`) |
| `save_to_memory(text)` | Append to a persistent long-term memory file |
| `set_alarm(hour, minute, tone_file?, enabled?)` / `stop_active_alarm` | Real, backed by `Services::AlarmService` |
| `mqtt_forward(topic, message)` | **Defined but not implemented.** `AppController::publishMqtt()` unconditionally returns `false`. No `MqttService`/`esp_mqtt` client exists in this codebase despite the Kconfig section and reserved SysDb component. |

## Depends on

`core_sysdb`, `audio_core`, `media_player`, `esp_websocket_client`,
`mbedtls` — see `CMakeLists.txt`.
