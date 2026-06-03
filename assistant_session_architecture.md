# Assistant Session Architecture Plan

> Status: Proposed device-side redesign for Gemini Live session control, LED state orchestration, and generated function-calling framework.

## 1. Goals

1. WebSocket lifecycle must be driven by assistant intent, not by Wi-Fi connection events.
2. Wake-word detection must remain responsive and must not block waiting for cloud connectivity.
3. Early user speech after wake-word should be buffered locally while the Gemini session is connecting.
4. Audio, transport, LED behavior, and error recovery must be coordinated by one central assistant state machine.
5. Function-calling must move to a generator-backed framework where generated code stays stable and handwritten logic lives in non-generated implementation files.

## 2. Current Architectural Problems

### 2.1 Session ownership is fragmented

- `AppController` starts Gemini transport on Wi-Fi connect.
- `AudioService` triggers reconnect behavior directly.
- `GeminiProtocolTask` owns transport state plus assistant turn parsing plus tool-call decoding.
- `LedService` reacts to low-level events rather than a high-level assistant state.

Result: there is no single place where the assistant conversation lifecycle is defined.

### 2.2 Transport is controlled from multiple threads

- Wake-word flow can call into websocket reconnect paths directly.
- WebSocket callbacks, protocol task code, and audio task code all influence session behavior.

Result: race-prone reconnect logic and unclear cleanup semantics.

### 2.3 Event contracts are too low-level

- `STOP_STREAMING`, `STREAMING_STOP_REQUESTED`, `ASSISTANT_TURN_COMPLETE`, and Wi-Fi events partially overlap in meaning.
- LED behavior is inferred from transport/audio internals instead of assistant intent.

Result: state transitions are hard to reason about and difficult to extend safely.

### 2.4 Tool-call pipeline is not generator-safe

- The current generator mixes generated contract code with runtime assumptions.
- Tool execution is effectively hardcoded in `AppController`.
- The event payload path for tool calls is fragile for future growth.

Result: adding new skills later will become more error-prone, not less.

## 3. Proposed Top-Level Design

Introduce a central service:

- `AssistantSessionService`

This becomes the single owner of:

- assistant session lifecycle
- websocket connect/close intent
- connect timeout and idle timeout policy
- buffering policy for pre-connect microphone audio
- recovery policy for Wi-Fi loss, quota issues, and server failures
- high-level LED state publication

The rest of the system becomes supporting layers:

- `WakeWordDetector`: detects wake-word, produces audio chunks, reports VAD/user interruption
- `AudioService`: only owns hardware/audio mode transitions and playback path coordination
- `GeminiTransportTask`: only owns websocket I/O
- `GeminiMessageParser`: converts JSON frames to typed Gemini events
- `ToolDispatcher`: maps decoded tool calls to generated interfaces and handwritten implementations
- `LedService`: renders one high-level assistant visual state

## 4. State Machine

### 4.1 Primary states

```text
Idle
StartingSession
Connecting
StreamingUserAudio
AssistantSpeaking
WaitingForFollowup
Closing
ErrorCooldown
```

### 4.2 State intent

- `Idle`
  Assistant is inactive. Wake-word detector is armed. No Gemini websocket is open.

- `StartingSession`
  Wake-word has fired. Assistant session request is accepted. Audio buffering is enabled immediately.

- `Connecting`
  WebSocket connection is being established. Buffered user audio is retained locally.

- `StreamingUserAudio`
  Connection is open. Buffered audio is flushed first, then live audio continues upstream.

- `AssistantSpeaking`
  Gemini is sending audio response. Playback is active. User interruption remains possible.

- `WaitingForFollowup`
  Assistant finished current turn, but the conversation window remains open for a short period.

- `Closing`
  Assistant session is winding down because of idle timeout, `goAway`, disconnect, or explicit abort.

- `ErrorCooldown`
  A classified failure occurred, and immediate reconnect should be suppressed for a short policy window.

### 4.3 State transitions

```text
Idle
  -> StartingSession          on WakeWordDetected with Wi-Fi available
  -> Idle                     on WakeWordDetected with Wi-Fi unavailable

StartingSession
  -> Connecting               immediately after session request accepted
  -> Idle                     on admission failure

Connecting
  -> StreamingUserAudio       on WsConnected
  -> Idle                     on ConnectTimeout
  -> Idle                     on WifiLost
  -> ErrorCooldown            on QuotaExceeded
  -> Idle                     on TransportFatal

StreamingUserAudio
  -> AssistantSpeaking        on AssistantAudioStarted
  -> WaitingForFollowup       on AssistantTurnComplete without active playback
  -> Closing                  on IdleTimeout
  -> Closing                  on GoAway
  -> Idle                     on WifiLost
  -> ErrorCooldown            on QuotaExceeded

AssistantSpeaking
  -> WaitingForFollowup       on AssistantTurnComplete
  -> StreamingUserAudio       on UserInterrupted
  -> Idle                     on WifiLost
  -> Closing                  on GoAway

WaitingForFollowup
  -> StreamingUserAudio       on UserSpeechDetected
  -> Closing                  on SessionIdleTimeout
  -> Idle                     on WifiLost

Closing
  -> Idle                     after playback drain and websocket close complete

ErrorCooldown
  -> Idle                     after cooldown timer elapses
```

## 5. Event Model

The state machine should consume typed assistant events rather than raw transport/audio details.

### 5.1 Input events

- `WakeWordDetected { channel }`
- `WifiAvailable`
- `WifiLost`
- `BufferedMicChunk { ptr/len or owned chunk }`
- `UserSpeechDetected`
- `VadTimeout`
- `UserInterrupted`
- `WsConnected`
- `WsConnectFailed { reason }`
- `WsClosed { reason }`
- `GeminiGoAway`
- `AssistantAudioStarted`
- `AssistantAudioChunk`
- `AssistantTurnComplete`
- `ToolCallReceived`
- `QuotaExceeded`
- `ServerError { code }`
- `TransportError { code }`
- `ConnectTimeout`
- `SessionIdleTimeout`
- `CooldownElapsed`

### 5.2 Output commands

- `TransportConnect`
- `TransportSendBufferedAudio`
- `TransportSendLiveAudio`
- `TransportClose`
- `AudioEnterConversationMode`
- `AudioEnterPlaybackMode24k`
- `AudioReturnToWakeMode16k`
- `AudioFlushPlayback`
- `AudioResumeMicStreaming`
- `AudioSuspendMicStreaming`
- `AssistantVisualStateChanged`

## 6. Service and File Boundaries

### 6.1 New device-side services

- `main/app/assistant/AssistantSessionService.h`
- `main/app/assistant/AssistantSessionService.cpp`
- `main/app/assistant/AssistantEvents.h`
- `main/app/assistant/AssistantVisualState.h`

Responsibilities:

- own state machine
- subscribe to Wi-Fi, wake-word, Gemini transport, and audio/session events
- publish assistant visual state
- publish transport commands
- own connect/session timers

### 6.2 Gemini transport split

Current `GeminiProtocolTask` should be split conceptually into:

- `GeminiTransportTask`
  Responsibilities:
  - websocket init/start/stop
  - send text/binary payloads
  - emit transport events only

- `GeminiMessageParser`
  Responsibilities:
  - parse inbound JSON
  - translate frames to typed events:
    - `AssistantAudioStarted`
    - `AssistantAudioChunk`
    - `AssistantTurnComplete`
    - `GeminiGoAway`
    - `ToolCallReceived`
    - `QuotaExceeded`
    - `ServerError`

`GeminiTransportTask` should not directly manipulate audio hardware state or LED state.

### 6.3 Audio path split

`AudioService` should remain the owner of:

- sample-rate transitions
- speaker pause/resume
- board audio handle reconfiguration
- playback drain coordination

It should stop owning:

- websocket reconnect decisions
- Gemini-specific session policy

### 6.4 LED ownership

`LedService` should subscribe to one high-level event:

- `AssistantVisualStateChanged`

Possible visual states:

- `Idle`
- `Listening`
- `Connecting`
- `Speaking`
- `Thinking`
- `Offline`
- `Recovering`
- `RateLimited`
- `Error`

This keeps LED behavior aligned with user perception, not internal subsystem noise.

## 7. Audio Buffering Strategy

When wake-word is detected:

1. session manager enters `StartingSession`
2. microphone buffering begins immediately
3. transport connect is requested
4. if websocket connects in time:
   - flush buffered audio
   - continue live uplink
5. if websocket does not connect in time:
   - discard buffered audio
   - return to `Idle`
   - re-arm wake-word path immediately

### 7.1 Buffer constraints

- fixed-size ring buffer in PSRAM
- enough for a short pre-connect window, such as 2 to 4 seconds
- overflow policy: drop oldest audio, never block wake-word/audio tasks

Suggested files:

- `main/app/assistant/PreconnectAudioBuffer.h`
- `main/app/assistant/PreconnectAudioBuffer.cpp`

## 8. Failure Taxonomy

Not all failures should be treated the same.

### 8.1 Offline

Examples:

- Wi-Fi unavailable at wake-word time
- Wi-Fi drops during session

Policy:

- fail fast
- restore idle audio mode immediately
- visual state: `Offline`

### 8.2 Quota/rate limit

Examples:

- Gemini returns 429 / quota exceeded

Policy:

- close active session
- enter `ErrorCooldown`
- suppress immediate reconnect storm
- visual state: `RateLimited`

### 8.3 Server-side recoverable error

Examples:

- transient server error
- `goAway`

Policy:

- close gracefully
- return to `Idle`
- allow next wake-word to create a fresh session

### 8.4 Transport fatal

Examples:

- websocket init failure
- repeated connection failures

Policy:

- abort current session
- optionally short cooldown
- visual state: `Recovering` or `Error`

## 9. Wi-Fi Integration Policy

Wi-Fi should no longer start Gemini transport proactively.

Wi-Fi should only publish availability events:

- `WifiAvailable`
- `WifiLost`

Assistant session logic decides whether to:

- accept a wake-word session request
- attempt a new websocket connection
- abort an active session

This decouples assistant lifecycle from network lifecycle while still honoring network reality.

## 10. Function-Calling Framework Plan

## 10.1 Principles

1. generated code must define contracts, not business logic
2. handwritten implementation files must survive regeneration
3. adding a new skill should not require editing core runtime files manually
4. transport schema generation and runtime dispatcher generation should come from the same source of truth

## 10.2 Proposed source-of-truth layout

Suggested skill metadata directory:

```text
main/app/gemini_live/skills/
  adjust_hardware_volume.skill.yaml
  get_current_weather.skill.yaml
```

Each skill spec should define:

- name
- description
- input fields
- output fields
- runtime handler name

### Example

```yaml
name: adjust_hardware_volume
description: Modifies physical speaker gain.
inputs:
  - name: volume_level
    type: int
    required: true
outputs:
  - name: status
    type: string
  - name: new_volume
    type: int
handler: AdjustHardwareVolumeHandler
```

## 10.3 Generated artifacts

The generator should emit:

- `gemini_skills_generated.h/.cpp`
  - Gemini handshake schema declarations
  - input/output structs
  - enum types
  - JSON decode/encode helpers

- `gemini_skill_dispatch_generated.h/.cpp`
  - dispatch table
  - registration helpers
  - stable handler interfaces

- optional stub files created once if missing:
  - `main/app/gemini_live/skills_impl/AdjustHardwareVolumeHandler.h`
  - `main/app/gemini_live/skills_impl/AdjustHardwareVolumeHandler.cpp`

## 10.4 Handwritten implementation boundary

Generated code calls stable handwritten handlers, for example:

```cpp
struct AdjustHardwareVolumeRequest {
    int volume_level = 0;
};

struct AdjustHardwareVolumeResponse {
    bool success = false;
    int new_volume = 0;
    const char* error_message = nullptr;
};

class IAdjustHardwareVolumeHandler {
public:
    virtual ~IAdjustHardwareVolumeHandler() = default;
    virtual bool handle(const AdjustHardwareVolumeRequest& request,
                        AdjustHardwareVolumeResponse& response) = 0;
};
```

The generator owns the request/response contracts.
The developer owns the implementation class.

## 10.5 Runtime dispatcher ownership

Introduce:

- `ToolDispatcherService`

Responsibilities:

- receive decoded tool-call events
- invoke the correct generated interface
- convert response struct to JSON
- send tool result back through Gemini transport

This keeps `AppController` out of tool business logic.

## 11. Recommended Refactor Sequence

### Phase 1: create the central assistant state model

- add `AssistantVisualState`
- add assistant-specific event definitions
- add `AssistantSessionService` skeleton with enum state only

Deliverable:

- a single place where session states and transitions are declared

### Phase 2: decouple Wi-Fi from websocket startup

- remove Gemini transport startup from Wi-Fi connect path
- convert Wi-Fi integration to availability events only
- make wake-word path request a session instead of forcing reconnect

Deliverable:

- websocket is no longer booted just because Wi-Fi came up

### Phase 3: single-owner websocket commands

- replace direct `forceReconnect()` calls with queued transport commands
- move connect/close control into transport task ownership

Deliverable:

- no websocket lifecycle mutation from audio/wake-word code

### Phase 4: buffered pre-connect audio

- add PSRAM pre-connect ring buffer
- feed buffered audio into transport after `WsConnected`

Deliverable:

- user can speak immediately after wake-word without losing the start of the utterance

### Phase 5: LED unification

- stop driving LED from transport/audio internals
- publish one assistant visual state

Deliverable:

- clearer and more predictable device behavior

### Phase 6: function-calling framework redesign

- move to skill metadata directory
- generate stable contracts and dispatch helpers
- add non-generated handler stubs
- move business logic out of `AppController`

Deliverable:

- easier future skill expansion with less manual glue code

### Phase 7: error classification and cooldown policy

- classify offline, quota, server, and transport errors separately
- add explicit cooldown state where needed

Deliverable:

- controlled recovery behavior instead of generic reconnect churn

## 12. Immediate Low-Risk Changes

These can be done first without full architecture migration:

1. Stop starting Gemini from Wi-Fi connect.
2. Introduce assistant visual state enum and publish it from current code.
3. Replace direct reconnect call with an assistant session request event.
4. Move tool execution out of `AppController` into a separate dispatcher service.

## 13. Success Criteria

The redesign is successful if:

1. Wake-word remains responsive even when cloud connectivity is bad.
2. WebSocket sessions only exist when an assistant session is active.
3. User speech immediately after wake-word is not lost during connect time.
4. LED behavior reflects assistant intent clearly.
5. New tools can be added by declaring metadata and implementing stable handlers, without editing core Gemini runtime code.
