# gemini_live

The voice assistant: the session state machine, the voice backend it talks
to (today Gemini Live, directly), and the tool router that lets the model
control the device.

## What's here

- **`VoiceAgent`** — the voice backend as the rest of the firmware sees it:
  mic audio and text turns up, tool responses, "ready", "reply owed",
  interrupt, barge-in, end of session. Everything reaches the backend through
  `VoiceAgent::active()`, which `main` sets once; a second backend (a hub
  relay) would be another implementation chosen there. It also routes the
  model's tool calls (board skills or remote MCP tools, error otherwise), so
  every backend shares one tool path. A backend reacts to
  `assistant.session_state`, reports `assistant.ws_state`, and writes the
  reply audio (24 kHz PCM) to `VOICE_RX_BUF`.
- **`GeminiProtocol`** — the direct backend: the WebSocket client (`esp_websocket_client` via
  the `WssClient` RAII wrapper) talking directly to
  `wss://generativelanguage.googleapis.com`, no proxy. The API key comes
  from NVS (`credentials::geminiApiKey()`), with the `GEMINI_API_KEY`
  Kconfig value as a fallback. Model, voice, system prompt and transcript
  options come from the settings in system.ndb (set from the dashboard),
  read at each session start.
  Gemini-only extras (session settings, the goAway test hook, the search
  refusal) stay on `GeminiProtocol`. Reply audio goes to the speaker path at
  24 kHz and is resampled to 32 kHz by `audio_core`.
- **`VoiceUplinkPump`** — Core 0 task sending the session's mic audio
  (16 kHz PCM from `MIC_TX_BUF`) to the backend.
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
| `set_alarm` / `set_timer` / `schedule` | Alarms, timers, scheduled reminders and actions (`main/services/alarm`, stored in system.ndb) |
| `list_schedule` / `cancel_scheduled(kind, id)` | List or delete them |
| `ringing_alarm(action)` | Stop or snooze the ringing alarm |
| `acknowledge_reminders` | Mark pending reminders heard |
| `get_weather(location, days)` | Open-Meteo; only offered when Google Search is off or refused |

The alarm, timer and reminder handlers live in
`main/services/alarm/AlarmTools.cpp`, reached through
`IDeviceCommandDelegate` (`AppController::handleAlarmTool`).

### Remote MCP Tools

In addition to the built-in tools, the voice backend supports dynamically
registering remote tools discovered from an external Model Context Protocol
(MCP) server (e.g. `nexus-mcp`). Discovered tools are converted to Gemini
function declarations (`parametersJsonSchema`) and appended to the setup
handshake using `PsramAllocator` to avoid internal RAM consumption. When
Gemini invokes a remote tool, the call is asynchronously dispatched to a
worker task that calls the remote MCP server via HTTP and returns the response.

## Known limits

- Gemini 3.x Live models reject the `googleSearch` tool with our key (no
  quota) and end the setup; Live 2.5 native-audio accepts it. The board
  reconnects without search on such a model for the rest of the boot, and
  the MCP `web_search` tool is offered instead.

## Depends on

`core_sysdb`, `audio_core`, `media_player`, `esp_websocket_client`,
`mbedtls` — see `CMakeLists.txt`.
