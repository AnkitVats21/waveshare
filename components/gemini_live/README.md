# gemini_live

The voice assistant: the session state machine and the voice backends
(Gemini Live directly, or through a hub). The tools themselves live in
`tool_registry` and in the features that register them.

## What's here

- **`VoiceAgent`** — the voice backend as the rest of the firmware sees it:
  mic audio and text turns up, tool responses, "ready", "reply owed",
  interrupt, barge-in, end of session. Everything reaches the backend through
  `VoiceAgent::active()`, which `main` sets once; a second backend (a hub
  relay) would be another implementation chosen there. It hands the model's tool calls to the
  `tool_registry` (`VoiceAgent::setToolRegistry`, wired in `main.cpp`), so
  every backend shares one tool path, and `toolRunning()` says whether a
  tool (the board's, MCP, or one the hub runs) is still to answer. A backend reacts to
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
- **`TranscriptLog`** — the last few turns of the conversation as text
  (from Gemini's input and output transcriptions), for
  `/api/assistant/transcript` and the dashboard.

## Tools

See `components/tool_registry/README.md`: the schema, the registry, and
which feature registers which tool. The session setup declares the
registered tools (`GeminiProtocol::buildSessionSetup`); the direct backend
adds the MCP server's tools after them.

**A running tool holds the session.** The model may say a line ("let me
check") and end its turn before a slow tool answers. While
`VoiceAgent::toolRunning()` is true (for at most `TOOL_WAIT_MS`, 60 s) the
silence timeout, the sound-without-words check and the follow-up timer
don't end the session; the answer is spoken when it comes. A hub tool counts
as running from the hub's `reply_owed` until that turn's `turn_complete`.

## Known limits

- Gemini 3.x Live models reject the `googleSearch` tool with our key (no
  quota) and end the setup; Live 2.5 native-audio accepts it. The board
  reconnects without search on such a model for the rest of the boot, and
  the MCP `web_search` tool is offered instead.

## Depends on

`tool_registry`, `core_sysdb`, `audio_core`, `sd_storage`, `credentials`,
`esp_websocket_client`, `mbedtls` — see `CMakeLists.txt`.
