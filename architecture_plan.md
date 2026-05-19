# Architecture Redesign Plan — Gemini Voice Relay Server

> **Status:** All design questions resolved. Ready for phased implementation.

## 0. Decisions Log

| # | Decision |
|---|----------|
| Q1 | **RTP**: Custom minimal hand-rolled header, configurable PT/clockRate/channels |
| Q2 | **Sample rates**: Local playback = 24 kHz (native Gemini), RTP to ESP = 16 kHz |
| Q3 | **Music vs AI audio**: Separate ring buffers, but RTP sender is **source-agnostic** — a `Mixer` feeds it |
| Q4 | **Alarms**: Server-side scheduler; plays through current `OutputMode` (local/rtp/both) |
| Q5 | **Bus**: Custom typed-channel bus — no external lib, zero reflection, Pi Zero 2W friendly |
| Q6 | **Audio Input**: RTP inbound from ESP (not raw UDP). See §3.5 |
| Q7 | **Keepalive**: Replace old heartbeat with RTP Comfort Noise (CN) packets — same protocol, tiny overhead |

---

## 1. Current State: Problems & Issues

### 1.1 The God-Object Anti-Pattern (`relay/server.go`)
`RelayServer` is the single most critical problem. It directly owns **every** subsystem and is responsible for the business logic of all of them:
- Holds concrete types (`*transport.UDPTransport`, `*ai.AIManager`, `*audio.Processor`, `*audio.AudioPlayer`, `*mqtt.Client`, `*db.Store`, `*webui.Server`, `*audio.Resampler`)
- Implements the AI `SessionHandler` interface itself (`OnAudioPart`, `OnTextPart`, `OnTurnComplete`)
- Manages the audio streaming pipeline (`streamToESP32`, `resampleOutputLoop`)
- Manages the session lifecycle watchdog (`sessionWatchdog`)
- Handles MQTT command routing (`handleMQTTCommand`)
- Runs the CLI (`cliLoop`)
- Publishes device state (`publishState`)
- Syncs device config to MQTT (`SyncDeviceConfig`)

**Result:** You cannot test, replace, or reason about any one of these without touching all the others.

### 1.2 Mutex Proliferation (Shared Mutable State)
The `RelayServer.mu` mutex is used to protect at least 5 independent pieces of state: `isResponding`, `streamEnabled`, `pcmPacketsSent`, `transport.LastClient()`, and the AI session. These have *nothing* to do with each other. This is a classic symptom of a shared-state architecture — every component needs to reach into every other component's state. The correct fix is to stop sharing state; instead, pass data via channels/events.

### 1.3 Ephemeral Skill Memory (`skills/memory.go`)
Memory is read from and written to a flat text file (`memory.txt`) via OS file I/O every time. There is a DB layer (`db/db.go`) that already has SQLite — but it is **never used by the skills layer**. Memory is not structured; it's one big text blob. Alarms have no persistence at all.

### 1.4 Raw UDP Transport (`transport/udp.go`)
- No sequence numbers → receiver can't detect packet loss or out-of-order delivery
- No timestamps → receiver can't reconstruct playback timing (jitter buffer is impossible)
- No SSRC → receiver can't distinguish multiple streams
- Custom byte-type framing (`0x01`–`0x08`) is not interoperable with any standard tool
- **Fix:** RTP (RFC 3550) solves all of the above with a standardized 12-byte header.

### 1.5 Audio Pipeline Issues
- `AudioPlayer` is hardcoded for local playback via `oto`. There is no concept of "output destination".
- `resampleOutputLoop` in `RelayServer` does real-time pacing with `time.Sleep` in a tight loop — this is fragile and couples timing logic into the relay layer.
- Music streaming (`skills/music.go`) calls `onAudio` callback directly from the mpv goroutine, bypassing any buffer or output management.
- The `Resampler` uses `sync.Cond` + mutex which is fine, but it is a simple in-memory FIFO, not a ring buffer. A fixed-size ring buffer (circular buffer) prevents unbounded memory growth under backpressure.
- No support for outputting to both local speakers AND RTP simultaneously.

### 1.6 Skills are Tightly Coupled to Transport/Audio
- `skills/hardware.go` imports `transport` to use `MsgBacklightOn` constants — a pure AI skill layer should not know about low-level wire protocol bytes.
- `skills/music.go` imports `audio` for `StreamConfig` and calls `exec.Command("mpv", ...)` directly. This means the music skill owns playback — the `AudioManager` doesn't.

### 1.7 Session Config Rebuilt on Every Wake-Word Event
Every wake-word in `processPackets` creates a new `genai.LiveConnectConfig` with all tools re-registered. The `skills.GetMemory()` reads from disk every time. This is wasteful and race-prone.

### 1.8 Things to Remove / Dead Code
- `audio/processor.go` — Only used when `SaveAudio = true` (which is `false` in `relay/config.go`). It's dead code in production. Move debug recording to a dedicated debug mode behind a CLI flag.
- `relay/interfaces.go` — Defines `Transport` and `AIHandler` interfaces but `RelayServer` uses concrete types. These interfaces are unused in practice.
- The `toggle` CLI command in `cliLoop` duplicates the MQTT `toggle_stream` command — one source of truth needed.
- `sessionWatchdog` double-checks the same `LastActivity()` condition twice (at 20s and 25s) with a no-op lock in between — logic bug.

---

## 2. Proposed Architecture: Event-Bus Driven

The core idea: **each subsystem is an autonomous goroutine. They communicate exclusively via typed events on a shared event bus. No component holds a direct reference to another (except the bus).**

```
┌─────────────────────────────────────────────────────────┐
│                     Event Bus                           │
│   (typed channels: AudioEvent, AIEvent, ControlEvent)  │
└──────┬──────┬──────┬──────┬──────┬──────┬──────────────┘
       │      │      │      │      │      │
   ┌───▼──┐ ┌─▼───┐ ┌▼────┐ ┌▼───┐ ┌▼───┐ ┌▼────────┐
   │  AI  │ │Audio│ │Trans│ │MQTT│ │ DB │ │  Web UI │
   │Mgr   │ │Mgr  │ │port │ │Mgr │ │Mgr │ │  Server │
   └──────┘ └─────┘ └─────┘ └────┘ └────┘ └─────────┘
```

### 2.1 Event Bus (`internal/bus/`)

```go
// EventType is a string constant identifying event kind
type EventType string

const (
    // AI layer events
    EvtAudioFromDevice  EventType = "audio.from_device"   // raw PCM from ESP32
    EvtAudioFromAI      EventType = "audio.from_ai"       // PCM response from Gemini
    EvtAISessionStart   EventType = "ai.session_start"    // wake word detected
    EvtAITurnComplete   EventType = "ai.turn_complete"    // Gemini finished speaking
    EvtAITextPart       EventType = "ai.text_part"        // transcript chunk

    // Audio output events
    EvtAudioOutputLocal EventType = "audio.output.local"  // play on local speakers
    EvtAudioOutputRTP   EventType = "audio.output.rtp"    // send via RTP
    
    // Control events
    EvtSetOutputMode    EventType = "ctrl.set_output_mode"  // local | rtp | dual
    EvtBacklightSet     EventType = "ctrl.backlight"
    EvtStopAssistant    EventType = "ctrl.stop"
    EvtDeviceConfig     EventType = "ctrl.device_config"
    EvtServerState      EventType = "ctrl.server_state"   // idle | listening | speaking

    // Skill events
    EvtMusicPlay        EventType = "skill.music_play"
    EvtAlarmSet         EventType = "skill.alarm_set"
    EvtMemoryUpdate     EventType = "skill.memory_update"
)

type Event struct {
    Type    EventType
    Payload any  // type-asserted by subscriber
}

type Bus struct {
    subscribers map[EventType][]chan Event
    mu          sync.RWMutex
}

func (b *Bus) Publish(e Event)
func (b *Bus) Subscribe(t EventType, bufSize int) <-chan Event
```

> **Why not a single `chan Event`?** Topic-per-subscriber means slow consumers only block themselves, not the whole bus. Each subscriber gets its own buffered channel.

---

## 3. Layer Breakdown

### 3.1 AI Layer (`internal/ai/`)

#### `AgentManager` (rename from `AIManager`)
Responsibilities:
- Manages Gemini Live session lifecycle (start, receive loop, idle timeout)
- Registers tools from the `SkillRegistry`
- Publishes events, consumes events from the bus
- Does **not** know about UDP/RTP, audio output, or MQTT

```
Bus → EvtAudioFromDevice  → AgentManager.SendAudio()
Bus → EvtAISessionStart   → AgentManager.StartSession()

AgentManager → Bus: EvtAudioFromAI (AI audio response)
AgentManager → Bus: EvtAITurnComplete
AgentManager → Bus: EvtAITextPart
AgentManager → Bus: Tool call results as EvtMusicPlay / EvtAlarmSet / etc.
```

**Memory on startup**: On `EvtAISessionStart`, the AgentManager queries `DB.GetMemory(deviceID)` from the DB layer (not a flat file).

#### `SkillRegistry` (was `ToolRegistry`)
- Each skill is registered with its tool definition AND its event type mapping
- When AI calls a tool, `SkillRegistry` publishes the appropriate bus event instead of calling a callback directly
- This removes ALL direct dependencies from skills → transport and skills → audio

```go
// Example: hardware skill no longer imports transport
// It publishes EvtBacklightSet{"state": "on"} to the bus
// The Transport layer subscribes to EvtBacklightSet and sends the wire message
```

#### Skills (`internal/skills/`)
Become **pure event publishers** — no imports of `transport`, `audio`, or concrete layers.

| Skill | Old | New |
|-------|-----|-----|
| `memory.go` | Writes to `memory.txt` | Publishes `EvtMemoryUpdate` → DB layer persists it |
| `alarm.go` | Stub | Publishes `EvtAlarmSet` → DB layer stores it; a scheduler goroutine fires it |
| `music.go` | Calls mpv, calls `onAudio` callback | Publishes `EvtMusicPlay{query}` → AudioManager handles playback |
| `hardware.go` | Imports `transport` constants | Publishes `EvtBacklightSet`, `EvtStopAssistant` |

---

### 3.2 Audio Layer (`internal/audio/`)

#### `AudioManager`
The single owner of all audio I/O. Replaces `AudioPlayer` + `Processor` + the streaming logic in `RelayServer`.

**Ring Buffer** (`internal/audio/ringbuf.go`):
- Fixed capacity (e.g., 2–4 seconds of audio = ~96KB at 16kHz/16bit/mono)
- Lock-free SPSC (single-producer, single-consumer) using atomic head/tail indices
- `Write([]byte) (int, error)` — drops oldest data on overflow (never blocks)
- `Read([]byte) (int, error)` — returns available data immediately (non-blocking)
- Two separate ring buffers: one for AI response audio, one for music audio

**Output Modes:**
```go
type OutputMode uint8
const (
    OutputLocal    OutputMode = 1 << 0  // play on host speakers via oto
    OutputRTP      OutputMode = 1 << 1  // send via RTP
    OutputDual     OutputMode = OutputLocal | OutputRTP
)
```

**AudioManager event subscriptions:**
```
EvtAudioFromAI   → write to AI ring buffer
EvtMusicPlay     → spawn mpv, write PCM to music ring buffer  
EvtSetOutputMode → switch OutputMode
EvtStopAssistant → drain/clear ring buffers
```

**AudioManager event publications:**
```
EvtAudioOutputLocal → (internal, consumed by local player goroutine)
EvtAudioOutputRTP   → (consumed by Transport layer's RTP sender)
```

**Internal goroutines:**
- `localPlaybackLoop()` — reads from ring buffer, writes to `oto.Player`
- `rtpDispatchLoop()` — reads from ring buffer, publishes chunks as `EvtAudioOutputRTP`
- These two loops run concurrently in `OutputDual` mode, both reading the same ring buffer (use a read-copy approach or dual-consumer design)

**Resampler:**
- Keep the existing pure-Go 24kHz→16kHz resampler for the RTP path
- Local playback stays at 24kHz (native Gemini output) — no resampling needed

---

### 3.3 Transport Layer (`internal/transport/`)

Replace raw UDP with **RTP (RFC 3550)**.

#### `RTPSender`
```go
type RTPSender struct {
    conn      *net.UDPConn
    ssrc      uint32          // Randomly chosen on start
    seqNum    uint16          // Increments per packet
    timestamp uint32          // Advances by samples per packet
    clockRate uint32          // e.g. 16000 for 16kHz
}
```

**RTP Packet structure (12-byte fixed header):**
```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
├─┬─┬─┬─┼─┴─┴─┴─┼─┼─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┤
│V=2│P│X│  CC   │M│      PT (96=L16)                │
├───┴─┴─┴────────┴─┴────────────────────────────────┤
│                   Sequence Number                  │
├───────────────────────────────────────────────────┤
│                     Timestamp                      │
├───────────────────────────────────────────────────┤
│                      SSRC                         │
└───────────────────────────────────────────────────┘
```
- **Payload Type 96** (dynamic, L16 mono 16kHz) — compatible with standard RTP receivers
- **Sequence Number** allows jitter buffer on ESP32 to detect loss and reorder
- **Timestamp** allows accurate timing reconstruction independent of network jitter

#### Control Messages
Keep a **separate UDP control socket** (or use RTCP) for non-audio messages (backlight, stop, heartbeat). This keeps audio-path latency clean.

#### Transport event subscriptions:
```
EvtAudioOutputRTP   → RTPSender.Send(payload)
EvtBacklightSet     → send control UDP packet
EvtStopAssistant    → send control UDP packet
```

#### Transport event publications:
```
Incoming UDP packet MsgStart → EvtAISessionStart{deviceID}
Incoming UDP packet MsgData  → EvtAudioFromDevice{pcm}
Incoming UDP packet MsgStop  → EvtStopAssistant
```

---

### 3.4 MQTT Layer (`internal/mqtt/`)
No major structural change needed, but wire it to the bus:

```
Subscribe "assistant/device/+/cmd" → publish EvtStopAssistant / EvtSetOutputMode to bus
Subscribe "assistant/device/+/config/ack" → publish EvtDeviceConfig{ack} to bus

Bus EvtServerState → Publish "assistant/server/assistant_state"
Bus EvtDeviceConfig{sync} → Publish "assistant/device/{id}/config"
```

---

### 3.5 DB Layer (`internal/db/`)
Extend to cover skill data:

**New tables:**
```sql
-- Persistent memory per device/user
CREATE TABLE IF NOT EXISTS memories (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT NOT NULL,
    content     TEXT NOT NULL,
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP
);

-- Alarms
CREATE TABLE IF NOT EXISTS alarms (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT NOT NULL,
    fire_at     DATETIME NOT NULL,
    label       TEXT,
    fired       BOOLEAN DEFAULT FALSE,
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP
);

-- Music cache metadata (was music_metadata.json)
CREATE TABLE IF NOT EXISTS music_cache (
    query       TEXT PRIMARY KEY,
    file_path   TEXT NOT NULL,
    cached_at   DATETIME DEFAULT CURRENT_TIMESTAMP
);
```

**DB layer subscribes to bus:**
```
EvtMemoryUpdate{deviceID, content} → INSERT INTO memories
EvtAlarmSet{deviceID, time, label} → INSERT INTO alarms
EvtMusicPlay (cache lookup/write)  → via AudioManager calling DB directly
```

**Memory loading on session start:**
`AgentManager` calls `db.GetMemories(deviceID)` → formats them into system instruction. No more flat file.

---

---

## 3.5 Bidirectional RTP & Session Keepalive (NEW)

This is the most important addition vs the original plan.

### Inbound RTP (ESP → Server)

```
Wake word detected on ESP
  → ESP starts sending RTP stream (16kHz, L16 mono, PT=96)
  → RTPTransport.readLoop() receives packets
  → Strips RTP header, publishes EvtAudioFromDevice{pcm}
  → AgentManager feeds PCM to Gemini session

Silence detected on ESP (after timer)
  → ESP stops sending RTP
  → RTPTransport detects inbound silence (no pkts for N ms)
  → Publishes EvtDeviceStreamEnd
```

### Outbound RTP (Server → ESP)

```
Gemini responds with audio
  → AudioManager writes to AI ring buffer
  → RTPSender reads chunks, builds RTP packets, sends to ESP
  → Each outbound RTP packet resets ESP silence-detection timer naturally

Gemini is silent (user still talking)
  → No AI audio in ring buffer
  → CN (Comfort Noise) goroutine sends a minimal RTP packet every ~1s:
        Header: PT=13 (CN), payload = 1 byte (noise level = 0)
        Size: 13 bytes total
  → This resets ESP's 3s silence timer → conversation stays alive

Server decides session is idle (e.g. 20s no activity)
  → Stop sending CN packets
  → ESP's silence timer fires naturally → ESP exits RTP mode
  → Server closes Gemini session
```

### Why CN over old UDP heartbeat

| | Old Heartbeat | CN Packets |
|---|---|---|
| Protocol | Custom byte `0x04` over UDP | Standard RTP PT=13 |
| Size | ~1 byte | 13 bytes |
| Logic | Separate goroutine + separate msg type | Part of RTP sender |
| ESP side | Custom handler | Single RTP parser handles all |
| Interop | None | RFC 3389 standard |

### RTPTransport — Bidirectional Design

```go
type RTPTransport struct {
    conn       *net.UDPConn  // single UDP socket, bidirectional
    ssrc       uint32        // outbound SSRC
    seqNum     atomic.Uint32
    timestamp  atomic.Uint32
    clockRate  uint32        // 16000
    payloadType uint8        // 96 for L16, 13 for CN

    lastClient atomic.Pointer[net.UDPAddr] // updated on each inbound packet
    inPackets  chan RTPPacket
    bus        *bus.Bus
}
```

Single `conn` on one port handles both directions (same as SIP/VoIP practice). The ESP sends from its port, server replies to that same address.

### Minimal RTP Header (hand-rolled, configurable)

```go
type RTPPacket struct {
    Version        uint8   // always 2
    Padding        bool
    Extension      bool
    Marker         bool    // set on first packet of a talk-spurt
    PayloadType    uint8   // 96=L16, 13=CN
    SequenceNumber uint16
    Timestamp      uint32  // advances by samples-per-packet
    SSRC           uint32
    Payload        []byte
}

func (p *RTPPacket) Marshal() []byte   // 12-byte header + payload
func UnmarshalRTP(b []byte) (*RTPPacket, error)
```

Configurable via `RTPConfig`:
```go
type RTPConfig struct {
    PayloadType uint8   // default 96
    ClockRate   uint32  // default 16000
    Channels    uint8   // default 1
    SSRC        uint32  // random on init
}
```

---

## 4. Application Boot (`cmd/server/main.go`)

```go
func main() {
    b := bus.New()

    dbStore  := db.NewStore("data/runtime")
    mqttMgr  := mqtt.NewManager(mqttCfg, b)
    agentMgr := ai.NewAgentManager(geminiClient, dbStore, b)
    audioMgr := audio.NewAudioManager(b)          // owns ring buffers + mixer
    rtpTrns  := transport.NewRTPTransport(rtpCfg, b) // bidirectional, single port
    webUI    := webui.NewServer(":8080", dbStore, b)

    ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
    defer cancel()

    for _, svc := range []Service{dbStore, mqttMgr, agentMgr, audioMgr, rtpTrns, webUI} {
        if err := svc.Start(ctx); err != nil {
            log.Fatalf("failed to start %T: %v", svc, err)
        }
    }
    <-ctx.Done()
}
```

**No `controlAddr` needed** — CN packets on the same RTP port replace the separate control socket for keepalive. True hardware control messages (backlight, stop) go over MQTT or a minimal separate 1-byte UDP control frame (not on the audio port).

---

## 5. Audio Pipeline: Full Picture

```
  ┌──────────── INBOUND (ESP → Server) ──────────────┐
  │  ESP RTP (16kHz L16 mono)                        │
  │  RTPTransport.readLoop()                         │
  │    → strips header → publishes EvtAudioFromDevice│
  │    → AgentManager.SendAudio() → Gemini           │
  └──────────────────────────────────────────────────┘

  ┌──────────── OUTBOUND (Server → ESP) ─────────────┐
  │                                                   │
  │  Gemini audio ──→ [AI RingBuf   ~96KB @ 24kHz]  │
  │  Music (mpv)  ──→ [Music RingBuf ~128KB @ 24kHz]│
  │  Alarm audio  ──→ [Alarm RingBuf ~32KB  @ 24kHz]│
  │                         ↓                        │
  │                  Mixer (add+clip, 24kHz)          │
  │                         ↓                        │
  │           ┌─────────────┴──────────────┐         │
  │           │                            │         │
  │    [Local @ 24kHz]             [Resampler]       │
  │     oto.Player                  24k→16k          │
  │    (RPi/laptop)                    ↓             │
  │                          [RTP RingBuf ~64KB]     │
  │                          RTPSender → ESP32       │
  │                    (+ CN pkts when idle)          │
  └───────────────────────────────────────────────────┘
```

**Output Modes:**

| Mode | Active goroutines |
|------|-------------------|
| `local` | `localPlaybackLoop` reads mixer output |
| `rtp` | `rtpDispatchLoop` reads mixer output → resamples → RTPSender |
| `local+rtp` | Both loops run; mixer output fan-out copies to each |

**Ring Buffer spec:**
- Fixed capacity byte arrays, pow-2 size
- `sync.Mutex` + `sync.Cond` for now (can go lock-free later)
- Overflow: drop oldest (real-time priority)
- Separate buffers per source → independent volume scaling before mixer

**Volume control (per source):**
```go
type Source struct {
    buf    *RingBuffer
    volume float32  // 0.0–1.0, applied in mixer before summing
}
```
Mixer multiplies each sample by its source volume before adding. This is how independent volume control works without complex DSP.



---

## 6. What Gets Removed

| Item | Action |
|------|--------|
| `audio/processor.go` | Remove (dead code, `SaveAudio = false`) |
| `relay/interfaces.go` | Remove (unused; interfaces live at bus boundary now) |
| `memory.txt` flat file | Replaced by `memories` table in SQLite |
| `music_metadata.json` | Replaced by `music_cache` table in SQLite |
| `RelayServer.mu` god-mutex | Eliminated — each manager has narrow own mutex |
| `relay/server.go` (as-is) | Decomposed into the 5 managers + bus |
| `relay/config.go` | Merge constants into appropriate packages |
| CLI `toggle` command | Replaced by bus event `EvtSetOutputMode` |
| `sessionWatchdog` double-check bug | Fixed as part of AgentManager's clean idle timeout |

---

## 7. Migration Strategy (Phased)

### Phase 1 — Event Bus + Decouple Transport
1. Create `internal/bus/` package with `Bus`, `Event`, `EventType`
2. Wrap `UDPTransport` to publish/subscribe on bus (no RTP yet, just events)
3. Wire MQTT client to bus
4. All `RelayServer` direct method calls become bus publishes

### Phase 2 — AI Manager + Persistent Memory
1. Refactor `AIManager` → `AgentManager` consuming bus events
2. Add `memories` and `alarms` tables to DB
3. Skills become event-only (no callbacks, no transport imports)

### Phase 3 — Audio Manager + Ring Buffer
1. Create `RingBuffer` in `internal/audio/ringbuf.go`
2. Refactor `AudioPlayer` + streaming logic into `AudioManager`
3. Implement `OutputMode` switching via bus event
4. Music skill publishes `EvtMusicPlay` → AudioManager owns mpv

### Phase 4 — RTP Transport
1. Implement `RTPSender` in `internal/transport/rtp.go`
2. Replace `UDPTransport.Send(MsgAudioData, ...)` with RTP packets
3. Keep control socket separate (or use RTCP SR for timing)
4. Update ESP32 receiver to parse RTP headers

### Phase 5 — Dual Output Mode
1. Fan-out the output ring buffer to both `LocalPlayer` and `RTPSender`
2. Switchable at runtime via bus event

---

## 8. Bus Implementation (Custom Typed-Channel)

Go 1.25 + Pi Zero 2W → **no external library**. Custom typed channels, zero reflection, compiler-checked.

```go
// internal/bus/bus.go
type EventType string

type Event struct {
    Type    EventType
    Payload any
}

type Bus struct {
    mu   sync.RWMutex
    subs map[EventType][]chan Event
}

func (b *Bus) Subscribe(t EventType, bufSize int) <-chan Event {
    ch := make(chan Event, bufSize)
    b.mu.Lock()
    b.subs[t] = append(b.subs[t], ch)
    b.mu.Unlock()
    return ch
}

// Publish is non-blocking — slow subscribers drop events
func (b *Bus) Publish(e Event) {
    b.mu.RLock()
    subs := b.subs[e.Type]
    b.mu.RUnlock()
    for _, ch := range subs {
        select {
        case ch <- e:
        default: // subscriber too slow, drop
        }
    }
}
```

Payloads are typed structs, type-asserted by each subscriber — safe and explicit.
