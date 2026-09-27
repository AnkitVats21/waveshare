# gemini_live

The voice assistant: a direct, bidirectional WebSocket connection to
Google's Gemini Live API, plus the tool/function-calling router that lets
Gemini actually control the device.

## What's here

- **`GeminiProtocol`** — the WebSocket client (`esp_websocket_client` via
  the `WssClient` RAII wrapper) talking directly to
  `wss://generativelanguage.googleapis.com`, no proxy. The API key comes
  from NVS (`credentials::geminiApiKey()`), with the `GEMINI_API_KEY`
  Kconfig value as a fallback. Model, voice, system prompt and transcript
  options come from the settings in system.ndb (set from the dashboard),
  read at each session start.
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
- **`TranscriptLog`** — the last few turns of the conversation as text
  (from Gemini's input and output transcriptions), for
  `/api/assistant/transcript` and the dashboard.
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

Current tools (25; `schema/gemini_skills_schema.json` is the source):

| Function | Purpose |
|---|---|
| `play(query)` / `play_next(query)` | Play now (clears the queue) / right after the current track |
| `pause` / `resume` / `stop` / `next` / `previous` | Playback transport |
| `volume(level)` / `mute` | Speaker volume (0-100) |
| `autoplay(enabled)` / `set_caching(enabled)` | Recommendations after the queue; saving streams to the card |
| `set_led_strip(r,g,b)` | Solid LED colour |
| `read_file(path)` / `write_file(path, content)` | Text notes, confined to `/sdcard/notes/` (plain file names only) |
| `save_to_memory(text)` | Append to a long-term memory file |
| `set_alarm` / `list_alarms` / `cancel_alarm` | Alarms (`main/services/alarm`, stored in system.ndb) |
| `stop_active_alarm` / `snooze_alarm` | The ringing alarm |
| `set_timer` | Countdown timer |
| `set_reminder` / `list_reminders` / `cancel_reminder` / `acknowledge_reminders` | Reminders |

The alarm, timer and reminder handlers live in
`main/services/alarm/AlarmTools.cpp`, reached through
`IDeviceCommandDelegate` (`AppController::handleAlarmTool`).

## Known limits

- Gemini 3.x Live models reject the `googleSearch` tool with our key and
  end the session; Live 2.5 native-audio accepts it. Search is therefore
  not declared.

## Depends on

`core_sysdb`, `audio_core`, `media_player`, `esp_websocket_client`,
`mbedtls` — see `CMakeLists.txt`.
