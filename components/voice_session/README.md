# voice_session

The voice session: its state machine and the one owner of what happens
after each reply. Built on `gemini_live` (the voice backends) and
`tool_registry`.

## What's here

- **`AssistantService`**: the session state machine (idle, connecting,
  listening, speaking, waiting for a follow-up, closing, error cooldown),
  the chimes, the connect and follow-up timers, the profile-switch restart.
  It carries out what `VoiceSession` decides.
- **`VoiceSession`**: the owner of the after-reply decision. It is told what
  happened (tool ends: it observes the tool registry; the hub's reports:
  `HubAgent` calls `onHubMusicDone()`), applies the rules and publishes the
  decision: `assistant.end_after_reply` in SysDb (close once the reply has
  played; read by `AssistantService` and `AudioService`) and `decision()`
  (the follow-up window). Nobody else writes `end_after_reply`.
- **`SessionRules`**: `voice::decide(facts, rules)`, a pure function, tested
  on the host (`host_tests/tests/test_session_rules.py`).

## Rules today

| After... | Then |
|---|---|
| a music command that succeeded, on the board or the hub | close once the reply has played |
| a music command that failed (not found, ambiguous, error) | stay open for the answer |
| anything else | wait for a follow-up (up to 60 s; the silence timeout usually ends it a few seconds after the reply), paused music stays paused |

While a tool runs for more than 400 ms, `VoiceSession` publishes
`assistant.tool` / `assistant.tool_kind`: the LED ring spins in the tool's
colour (search cyan, music purple, Bluetooth blue, other white; not while
the assistant speaks) and the dashboard can say what is happening. A hub
tool shows with the name and kind the hub sends in `reply_owed` (older
hubs: `tool` / `other`).

## Errors and the fallback model

A backend reports why a session failed (`VoiceAgent::setErrorSink`: the
close code and reason, Gemini's own or the hub's `session_error`).
`voice::classifyError()` sorts it into `quota`, `auth`, `model`, `network`
or `other`, published as `assistant.error` until the next session starts;
the LED shows a quota error orange instead of red. After a quota error the
next sessions use `Rules::fallback_model` (gemini-3.1-flash-live-preview)
for `fallback_minutes` (60), unless that is the model that failed.

`voice::Rules` holds the tunables (follow-up window, whether a failed music
command also closes, off by default); defaults are compiled in.

A running tool holds the session whatever the rules say: see
`VoiceAgent::toolRunning()` in `gemini_live`.
