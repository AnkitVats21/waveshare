# Architecture

The target design of the firmware, and the rules that keep new work inside
it. The aim is one design that new features fit into without restructuring:
each part has one owner, its dependencies are visible, and new capabilities
are added by registering them, not by editing a central `switch`.

Status (2026-11): sections 1 and 2 describe what exists and stays. Sections
3 to 5 are the target; section 6 is the order of work (steps 1 to 4 done). Section 7 (loadable
modules) is a later step that depends on a feasibility test.

## 1. What stays

These parts are sound and are the base for everything else.

**Layers without cycles.** Components depend downward only:

```
                 ui_view
                    │
   gemini_live  media_player ──► nexus_db ──► sd_storage ◄── credentials
        │           │
        └──► audio_core
                    │
                core_sysdb          http_server (standalone)
```

`main/` is the application on top: it wires the components together and
holds the services (alarms, HTTP routes, hub link, MCP, profiles).

**One state store.** `EmbeddedSysDb` holds all live state, generated from
`schema/sysdb.star`. Services change it with `mutate()`; tasks that care are
woken by per-field change bits (`ReactorTask`). Features talk through state,
not by calling each other.

**One voice interface, two backends.** `VoiceAgent` is the voice backend
for a session: `GeminiProtocol` (straight to Gemini, standalone) or
`HubAgent` (through a LAN hub). Tool calls are routed in the base class
(`VoiceAgent::dispatchToolCall`), so both modes run the same tool code.

**Interfaces at the hardware and audio seams.** `IAudioSink`,
`IAudioDecoder`, `IResampler`, `IWakeWordListener`, `IAudioFocusObserver`,
`IDeviceCommandDelegate` and others.

**Safe updates.** Two firmware slots and two dashboard slots, each with
rollback (section 8).

## 2. Two modes

| | Standalone | Hub mode |
|---|---|---|
| Gemini connection | the board (`GeminiProtocol`) | the hub; the board streams audio (`HubAgent`) |
| Who owns the music | the board (`MusicPlaybackService`) | the hub; the board is one of its players |
| Tools | the board's own, plus MCP | the board's own, MCP and the hub's (music, Bluetooth) |

**The board owns the conversation in both modes.** It holds the mic, the
wake word, voice detection, the follow-up window, the LED and its speaker,
so it decides what happens after each reply. The hub only reports facts the
board can't see (which hub tool ran and how it ended, Gemini's errors) and
carries out what the board asks of hub-side things (music on a hub
speaker). The decision logic therefore exists once, on the board.

## 3. Three rules

### Rule 1: dependencies are visible

- A component's `CMakeLists.txt` lists what it uses (`REQUIRES` /
  `PRIV_REQUIRES`). An include it doesn't declare fails the build.
- New code receives the objects it uses (constructor or `init()`
  arguments), wired in `main/main.cpp`. It doesn't call `getInstance()`.
  Existing singletons stay until their code is changed for another reason.
- Components never include `main/` headers. When a component needs the
  application, it declares an interface that `main/` implements (as
  `IDeviceCommandDelegate` does).
- New code includes headers by their component path
  (`media_player/NexusPlayer.h`), not the old forwarding paths.

### Rule 2: extend by registering

Capabilities that grow over time are kept in registries, not in `switch`
statements spread over files:

- **Tools** (section 4.2): each tool registers its declaration, kind and
  handler. Adding a tool touches one file.
- **Settings** that tune behaviour (follow-up windows, quiet-music level,
  fallback model) are data in `system.ndb`, editable from the dashboard,
  not constants.
- **Modules** later plug into the same registries (section 7).

### Rule 3: one owner per decision

Each decision has one component that makes it; others send it events or
read the state it publishes. Shared flags that several components set to
steer one behaviour are replaced by events to the owner. (The end of a
session was steered by a `media_pending_idle` flag that three components
set and cleared; it is now `end_after_reply`, written by `voice_session`
only.)

| Decision | Owner |
|---|---|
| Session state, follow-ups, what the music does around a reply, tool progress, voice errors and model fallback | `voice_session` |
| Who has the speaker (voice, music, alerts, alarm) | `AudioOrchestrator` |
| The queue and what plays | `MusicPlaybackService` (standalone) or the hub |
| Alarms, timers, reminders | `AlarmService` |

## 4. The voice area (target)

Every upcoming voice feature lands here, so this is the part restructured
first (section 6).

```
  wake word / keys / text turns
              │
              ▼
  ┌─────────────────────────────── voice_session ─────────────────────────┐
  │ state machine · follow-up window · rules table · tool progress ·       │
  │ errors and model fallback · music around replies                       │
  └──────▲──────────────────────────────┬──────────────────────────────────┘
         │ events                       │ commands
  ┌──────┴──────────┐            ┌──────▼─────────┐     ┌──────────────────┐
  │ VoiceAgent      │──tool call─► tool_registry  │────►│ tool handlers    │
  │  GeminiProtocol │◄─result────│ local · MCP ·  │     │ (device, music,  │
  │  HubAgent       │            │ hub-owned      │     │  alarms, notes…) │
  └─────────────────┘            └────────────────┘     └──────────────────┘
         │                              │
     gemini_live                 publishes assistant.tool, assistant.error
   (protocol only)                      to EmbeddedSysDb (LED, dashboard)
```

### 4.1 Components

| Component | Holds | Status |
|---|---|---|
| `gemini_live` | the Gemini Live protocol only: `GeminiProtocol`, `WssClient`, uplink pump, the `VoiceAgent` interface | done |
| `voice_session` | `AssistantService` (the state machine), the rules, the follow-up window, tool progress, error kinds and fallback | state machine, rules and follow-up window done; tool progress and errors to come |
| `tool_registry` | the tool list, declarations for the session setup, routing a call to its handler | done |
| `hub_link` | `HubLink`, `HubAgent`, `HubMusic`; settings and the dashboard relay injected by main | done |
| tool handlers | where their feature lives (music tools in `media_player`, device tools in `services/tools`, alarm tools in `services/alarm`, …) | done |

### 4.2 Tool registry

A tool is:

```cpp
struct Tool {
    const char* name;
    const char* declaration;   // Gemini function declaration (JSON)
    ToolKind kind;             // search, music, bluetooth, info, device, other
    ToolOwner owner;           // local, mcp, hub
    void (*handle)(const ToolCall& call, ToolReply& reply, void* ctx);
    void* ctx;
};
```

- Local tools register at startup from the component that owns the
  feature. MCP tools are registered by `McpService` when it lists the
  server's tools. In hub mode the hub owns some tools; the board doesn't
  declare those.
- The session setup's tool declarations are assembled from the registry at
  session start, not baked into a generated string.
- Every handler answers with a `status`: `success`, `ambiguous`,
  `not_found` or `error`. `voice_session` uses it to decide what happens
  after the reply.
- Slow handlers answer later (`reply` is completed from another task), as
  `get_weather` does today.
- The schema (`gemini_skills_schema.json`) may stay as the source of
  declarations and argument checks; the generated code then fills the
  registry instead of a `switch`.

### 4.3 voice_session

Inputs are events, from either backend and from the registry:

| Event | From |
|---|---|
| `SessionReady`, `ReplyStarted`, `ReplyEnded`, `TurnComplete`, `Interrupted` | `VoiceAgent` |
| `ModelText` (the reply's transcript) | `VoiceAgent` |
| `ToolStarted {name, kind}`, `ToolEnded {name, kind, status}` | `tool_registry` (local and MCP tools), `HubAgent` (hub tools) |
| `SessionError {kind, retry_after}` | `VoiceAgent`; the hub passes Gemini's close code and reason through, and one function sorts both into kinds (`quota`, `auth`, `model`, `network`, `other`) |
| `UserSpeech` | the audio service (voice detection) |

After each reply it looks up the **rules table** (data, with defaults built
in) to decide: end the session or wait for a follow-up, for how long, and
what the music does meanwhile (stay paused, play quietly, play). It
publishes `assistant.tool` and `assistant.error` for the LED and the
dashboard, and asks the hub to do the same for music on a hub speaker.

The rules are a pure function and are tested on the host.

## 5. Data over code

What is tuned often lives in settings, not in code, so changing it needs
neither a build nor a flash:

- the model, the fallback model, the voice, the prompt;
- the rules table and its timings (follow-up windows, quiet level);
- which optional tools are on.

Defaults are compiled in; the stored values override them; a value that
fails validation falls back to the default.

## 6. Order of work

One restructure of the voice area, then features. Code outside it (alarms,
HTTP, media, storage) moves to the rules only when it is changed for
another reason.

1. **Make dependencies explicit.** `REQUIRES` in every component; the build
   must still pass. No behaviour change.
2. **`tool_registry`.** Registry and `Tool`; the generated decoder and the
   MCP hook fill it; `VoiceAgent::dispatchToolCall` routes through it;
   handlers move next to their features. Host tests for routing.
3. **`voice_session`.** Move `AssistantService` out of `gemini_live`; replace
   the session flags with events; rules table with host tests.
   `gemini_live` keeps only the protocol.
4. **`hub_link`.** Move `main/services/hub` into a component.
5. **Features on top:** tool progress (LED and `assistant.tool`), the
   follow-up and music rules, status tools, error kinds and model fallback,
   with the matching hub messages.

Each step builds, passes the host tests and is checked on the board before
the next one.

## 7. Loadable modules (later)

Goal: change a feature without a flash or a reboot, by downloading a
module and loading it into PSRAM.

- **Mechanism:** Espressif's `elf_loader` component, which can run code
  from PSRAM on the ESP32-S3.
- **Interface:** the core passes a module one versioned table of functions
  ("host API v1": log, read and change state, register a tool, set the LED,
  schedule work). Modules don't link against firmware symbols directly and
  no C++ objects cross the boundary.
- **Matching:** a module states `{name, version, api}`; the core loads it
  only if the API version matches.
- **Delivery:** from the hub, kept on the SD card, never written to flash.
  When a module isn't available, the built-in version runs.
- **Safety:** there is no memory protection between a module and the core.
  The core records which module it is loading; after a crash or watchdog
  reset during or after that load, the next boot skips the module and runs
  the built-in one.
- **Candidates:** `voice_session` rules, tools, briefings. Not the real-time
  audio path, the wake word or Wi-Fi.
- **Gate:** a feasibility test on the board first. Memory protection is on
  and locked (`CONFIG_ESP_SYSTEM_MEMPROT`), and code already runs from PSRAM
  (`CONFIG_SPIRAM_XIP_FROM_PSRAM`); the test shows whether the loader works
  with both, how long a load takes and what it costs in memory.

Modules save little memory: the firmware's code already sits in PSRAM
(about 2.6 MB), and modules only spare the code of features that aren't
loaded. Internal RAM isn't helped at all.

## 8. Updates

| What | How |
|---|---|
| Firmware | OTA into the other app slot (`ota_0` / `ota_1`), rollback if the new build doesn't confirm itself; `/api/ota` |
| Dashboard | its own two slots (`www_0` / `www_1`), with rollback; `/api/ota/frontend` |
| Settings and rules | data, applied at the next session |
| Later: delta firmware | only the difference to the running build is sent; the patch applies only to the build it was made from |
| Later: modules | section 7 |

## 9. Decisions

| Decision | Why |
|---|---|
| The board decides the conversation in both modes; the hub reports facts | one copy of the logic; the board owns the mic, speaker and LED anyway |
| No runtime modules yet | a single board that can reboot in seconds while idle gains little; the registries and the host API keep the option open |
| Refactor the voice area only, the rest when touched | every planned feature lands there; a full rewrite risks the audio paths for no gain |
| Rules and tuning as data | changes without a flash, and the same rules can later move into a module |
| Code keeps running from PSRAM | flash writes don't stall the cache (see AGENTS.md); its PSRAM cost is accepted |
