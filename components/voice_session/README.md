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
| a music command on the board (any status) or on the hub | close once the reply has played |
| anything else | wait 60 s for a follow-up, paused music stays paused |

`voice::Rules` holds the tunables (follow-up window, whether a failed music
command also closes); defaults are compiled in.

A running tool holds the session whatever the rules say: see
`VoiceAgent::toolRunning()` in `gemini_live`.
