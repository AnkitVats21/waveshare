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

Current tools (15; `schema/gemini_skills_schema.json` is the source). Related
actions share one tool with an `action`/`kind` parameter: every declaration is
sent with each session, and the model picks less reliably among many tools.

| Function | Purpose |
|---|---|
| `play(query, when)` | Play now (clears the queue) or next |
| `playback(action)` | pause / resume / stop / next / previous |
| `volume(level)` | Speaker volume, 0-100 (0 mutes) |
| `music_settings(autoplay, caching)` | Recommendations after the queue; saving streams to the card |
| `notes(action, ...)` | list / read / write / append / delete text notes in `/sdcard/notes/` (plain names; a delete needs a confirmed second call after the user agreed) |
| `set_led_strip(r,g,b)` | Solid LED colour |
| `save_to_memory(text)` | Append to a long-term memory file |
| `set_alarm` / `set_timer` / `set_reminder` | Alarms, timers, reminders (`main/services/alarm`, stored in system.ndb) |
| `list_schedule` / `cancel_scheduled(kind, id)` | List or delete them |
| `ringing_alarm(action)` | Stop or snooze the ringing alarm |
| `acknowledge_reminders` | Mark pending reminders heard |
| `get_weather(location, days)` | Open-Meteo; only offered when Google Search is off or refused |

The alarm, timer and reminder handlers live in
`main/services/alarm/AlarmTools.cpp`, reached through
`IDeviceCommandDelegate` (`AppController::handleAlarmTool`).

### Remote MCP Tools

In addition to the 15 built-in tools, `GeminiProtocol` supports dynamically
registering remote tools discovered from an external Model Context Protocol
(MCP) server (e.g. `nexus-mcp`). Discovered tools are converted to Gemini
function declarations (`parametersJsonSchema`) and appended to the setup
handshake using `PsramAllocator` to avoid internal RAM consumption. When
Gemini invokes a remote tool, the call is asynchronously dispatched to a
worker task that calls the remote MCP server via HTTP and returns the response.

## Known limits

- Gemini 3.x Live models reject the `googleSearch` tool with our key and
  end the session; Live 2.5 native-audio accepts it. Search is therefore
  not declared.

## Depends on

`core_sysdb`, `audio_core`, `media_player`, `esp_websocket_client`,
`mbedtls` — see `CMakeLists.txt`.
