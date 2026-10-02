# Plan: Hub mode (voice relay and music control on a LAN hub)

Status: **proposed**, for review before any code
Last updated: 2026-10-02

---

## 1. What this is

An optional second way for the board to work, beside today's direct mode.
A hub on the home network (first a PC, then a Raspberry Pi Zero 2W) holds
the conversation with Gemini and the music queue. The board stays the
microphone, the speaker, the wake word, the music player and the clock.

| | Direct mode (today) | Hub mode |
|---|---|---|
| Wake word, echo cancellation | Board | Board |
| Conversation with Gemini | Board, TLS WebSocket to Google | Hub; board talks to the hub |
| Voice audio to the board | ~64 KB/s base64 JSON | Opus, ~3-4 KB/s, paced |
| Music queue, search, recommendations | Board | Hub |
| Music bytes (stream, decode, card cache, seek) | Board | Board |
| Alarms, timers, reminders | Board | Board |
| Dashboard | Served by the board, one client | Served by the hub, many clients |
| Orbit satellites | Connect to the board | Connect to the hub |

Both modes stay. Hub support is a build option, and the mode is a setting that
can change at run time; in `auto`, the board uses the hub when it is up and
falls back to direct mode when it isn't.

Decisions taken (2026-10-02):

- The hub runs on the LAN (PC first, then the Pi). Lightsail as a hub is not
  part of this plan.
- Music: the data path stays on the board; control moves to the hub.
- Mic uplink format is negotiated and can be set (PCM or Opus).
- The board keeps a copy of the next few queue entries, so music carries on
  when the hub drops.
- The hub is written in Go, grown from nexus-orbit (its coordinator and the
  Orbit messages already exist).

## 2. Why

- **Voice glitches and RAM.** Gemini's reply arrives as base64 JSON, about
  64 KB/s, through a TLS connection with a 5.7 KB TCP window. That is close
  to real time, so a lost packet is an audible gap, and the TLS session costs
  internal RAM. From the hub the reply is Opus at a few KB/s, sent slightly
  ahead of playback, over plain `ws://` on the LAN.
- **Less work on the board.** Session resumption, goAway handoffs, the
  "reconnect to drop the queued reply" interrupt, MCP calls for voice: all of
  it moves to the hub.
- **More than one of everything.** Many dashboard clients, satellites,
  later several boards (rooms).
- **Wake to answer.** The board keeps one open connection to the hub and the
  hub can keep Gemini warm, so a session starts with one message instead of a
  TLS handshake and a setup.

## 3. Board side

### 3.1 Wake word and the audio path (unchanged)

The wake word stays on the board. The AFE's echo cancellation needs the
signal the board sends to its own speaker, and only the board has it; because
music, voice and chimes are all mixed on the board, all of them are
cancelled, whatever their source. No audio leaves the board before the wake
word, as today.

Unchanged in hub mode: `WakeWordEngine`, `MicCapture`, `MIC_TX_BUF`,
`VOICE_RX_BUF` (24 kHz PCM, resampled to 32 kHz), `SpeakerPlayback`,
`AudioOrchestrator` (ducking and pausing music under voice happen on the
board, at once), chimes, LEDs, `AssistantService`'s state machine and
timeouts.

### 3.2 New and changed parts

| Part | Where | What |
|---|---|---|
| `HubLink` | `main/services/hub/` | The board's one connection to the hub: `ws://` client, hello, heartbeat, reconnect with backoff, routes messages by type and binary frames by channel. Reports `hub_up`. |
| `HubAgent : VoiceAgent` | `main/services/hub/` | The voice backend over `HubLink`. Mic audio up (PCM or Opus), reply audio down (Opus, decoded into `VOICE_RX_BUF`), tool calls in, tool responses out, interrupt, text turns. |
| Backend choice per session | `gemini_live` | Today `VoiceAgent::active()` is fixed at boot. It becomes a choice made when a session starts (`direct`, `hub` or `auto`). Both backends react to `assistant.session_state`, so each one acts only while it is the active one. |
| Voice decoder | `HubAgent` | An Opus decoder (24 kHz mono, state in PSRAM). The board already decodes Opus for music. |
| Uplink encoder | `HubAgent` | Optional Opus encoder (16 kHz mono, `RESTRICTED_LOWDELAY`, as the recorder uses), on core 0. Used only when the hub picks Opus. |
| Music control in hub mode | `media_player` | `MusicPlaybackService` gets a control mode. Local (today): keys, dashboard and voice act on the board's queue. Hub: those actions go to the hub, and the board plays what the hub sends. It reuses the remote-control paths written for Orbit, the other way round. |
| State push | `main/services/http` | The JSON that `/api/ws` pushes today is built by a shared function, sent to the dashboard socket and to the hub. |
| Settings | `schema/db/system.star` | See 3.4. |
| Hub token | `credentials` (NVS) | Shared secret for the hello, imported like the Gemini key, never in system.ndb or logs. |

Removed from the board in hub mode at run time: nothing. Direct mode must
still work as the fallback, so its code stays. The build option only adds the
hub code (`CONFIG_WAVESHARE_VOICE_HUB`, default off until it is proven).

### 3.3 The HTTP server

The board's HTTP server stays, in both modes. It is the recovery path when
the hub is down: setup portal, OTA, logs, SD files, and the dashboard served
from flash.

In hub mode the hub is the front door:

- The hub serves the same dashboard build.
- The hub forwards `/api/*` calls to the board's HTTP server (a reverse
  proxy; both are on the LAN). The board's 89 routes are not rewritten and
  the board needs no request tunnel. Large transfers (OTA, files,
  recordings) stream through the proxy unchanged.
- Live state: the board pushes it to the hub over `HubLink`; the hub fans it
  out to every browser on its own socket. This lifts the one-client limit of
  the board's `/api/ws`, which stays for direct mode.
- `/api/orbit/ws` stays on the board for direct mode; in hub mode satellites
  connect to the hub.

Possible later saving: fewer open sockets on the board in hub mode
(`max_open_sockets` is 12), once measured.

### 3.4 Settings

New fields in the `settings` document (system.ndb), editable from the
dashboard:

| Field | Values | Meaning |
|---|---|---|
| `voice_backend` | `direct`, `hub`, `auto` | Which backend a session uses. `auto`: the hub when `hub_up`, else direct. |
| `hub_url` | string | `ws://host:port/...`; empty = find it by mDNS (`_nexus-hub._tcp`). |
| `uplink_codec` | `auto`, `pcm`, `opus` | `auto` lets the hub pick. |
| `uplink_opus_kbps` | 16-32 | Opus bitrate when Opus is used. |
| `uplink_opus_complexity` | 0-10 | Opus encoder complexity (CPU on core 0). |

The Gemini settings (model, voice, system prompt, VAD, transcripts) stay
where they are. In hub mode the board sends them with each session start,
so system.ndb stays the one place they are set.

## 4. The connection and its messages

One WebSocket from the board to the hub. JSON text frames for control
(`{"type": ...}`), binary frames for audio.

Binary frame: byte 0 is the channel (1 = mic up, 2 = voice down), byte 1 is
flags (bit 0 = last frame of a reply), then one or more length-prefixed
packets (2-byte length, then an Opus packet or a PCM chunk). Downlink sends
about 60 ms per frame to keep the frame count down.

### 4.1 Connecting

| From | Message | Content |
|---|---|---|
| board | `hello` | device id, name, firmware version, token, formats it can send and play, hash of its skill declarations |
| hub | `welcome` | hub version, chosen uplink format (codec, bitrate, complexity), downlink format, whether it controls music, `need_skills` if the hash is unknown |
| board | `skills` | the board's tool declarations (generated from `gemini_skills_schema.json`), only when asked |
| both | `ping` / `pong` | every 5 s; no answer for 10 s marks the link down (a closed socket marks it down at once) |

### 4.2 Voice

| From | Message | Content |
|---|---|---|
| board | `session_start` | why (wake, button, API, scheduled), quiet or not, the session settings, an optional first text turn |
| hub | `session_ready` | the hub's Gemini session accepted the setup (`VoiceAgent::ready()`) |
| board | mic audio (channel 1) | 16 kHz mono, PCM or Opus as agreed |
| hub | reply audio (channel 2) | Opus 24 kHz mono, sent a little ahead of real time (about 300 ms) |
| hub | `reply_owed` | true or false (`awaitingReply()`): the hub knows when Gemini owes a reply |
| hub | `turn_complete` | the model's turn is over |
| hub | `interrupted` | Gemini dropped the rest of its reply |
| board | `interrupt` | the person talked over the reply; the hub stops sending and drops its queue |
| hub | `tool_call` | id, name, args, for board skills only |
| board | `tool_response` | id, result |
| board | `text_turn` | a text turn (reminders, briefings, the API) |
| hub | `transcript` | turn text, for `TranscriptLog` and the dashboard |
| board | `session_end` | the session is over; the hub may keep Gemini warm |
| hub | `session_error` | reason; the board plays the error chime and ends the session |

MCP tools are called by the hub itself. Session resumption, goAway handoffs
and Google Search refusals are the hub's business.

### 4.3 Music (hub controls, board plays)

| From | Message | Content |
|---|---|---|
| hub | `music_play` | track (id, title, artist, duration), stream URL, position, the next few queue entries |
| hub | `music_pause`, `music_resume`, `music_seek`, `music_stop` | as Orbit |
| hub | `music_queue` | updated copy of the next few entries |
| hub | `music_url` | a renewed stream URL (answer to `music_renew`) |
| board | `music_report` | state, position, track id: on change and every second while playing |
| board | `music_event` | `track_ended` or `error` (with reason) |
| board | `music_action` | key presses and other local actions: next, previous, toggle, play a query |
| board | `music_renew` | the stream URL of a track is about to expire (today's `RENEW_URL`) |

Stream URLs are resolved by the hub through nexus-mcp `music_track`. They
are bound to the home IP, which is also the board's, so the board plays them
as today. A song already saved on the card plays from the card (the existing
check in the player).

### 4.4 State

| From | Message | Content |
|---|---|---|
| board | `state` | the same JSON `/api/ws` pushes today, on change, at most twice a second |

## 5. Hub side

A Go service (grown from nexus-orbit), one process:

- **Board endpoint.** Accepts board connections, checks the token, keeps one
  session object per board.
- **Gemini session manager.** One Gemini Live session per board: connects on
  `session_start` (or keeps one warm), handles resumption and goAway on its
  own, sends the board's settings and skill declarations plus MCP tools.
- **Audio.** Decodes Opus mic audio to 16 kHz PCM for Gemini when Opus is
  used; encodes Gemini's 24 kHz PCM to Opus; paces the downlink; drops its
  queue on an interrupt.
- **Tools.** Board skills go to the board as `tool_call`; MCP tools go
  straight to nexus-mcp.
- **Music controller.** Queue, search, recommendations through nexus-mcp,
  outputs (the board or an Orbit satellite). Voice `play`, `playback` and
  `music_output` are handled here.
- **Orbit satellites.** The existing coordinator, now on the hub.
- **Dashboard.** Serves the bundle, proxies `/api/*` to the board, fans out
  the board's state to browsers over its own socket.
- **Config.** Gemini key, MCP URL and token, board tokens: files on the hub,
  never in the repository.

The Pi Zero 2W (4 cores, 512 MB) should manage one board: Opus at 24 kHz mono
is a few percent of a core. To be measured in phase 5.

## 6. Failover

| Event | What happens |
|---|---|
| Hub down at wake, `voice_backend = auto` | The session goes direct (needs the Gemini key on the board). The choice is instant: `hub_up` is known from the heartbeat. |
| Hub down at wake, `voice_backend = hub` | Error chime, as today when offline. |
| Hub drops during a session | The session ends with the error chime; the next wake picks again. No switch in the middle of a conversation. |
| Hub drops during a song | The song keeps playing (its bytes come from YouTube or the card). At its end the board plays the queue copy it holds. |
| Hub comes back | The board reports its state and position; the hub takes control again from there (the board's current track wins). |
| Hub drops with satellites playing | Satellites stop; music is not moved back to the board automatically in the first version. |

## 7. Measurements

Measured in direct mode first (phase 0) and again in hub mode:

- Internal RAM: free when idle, during a session, during a session over
  music, lowest point (`/api/system/metrics`).
- Wake to "ready" (`session_ready` or Gemini's `setupComplete`) and wake to
  the first reply audio.
- Voice glitches: the per-turn underrun and frame-gap counters already in
  `SpeakerPlayback` and `GeminiProtocol`.
- Core 0 CPU with Opus uplink at a few complexities.
- Hub CPU and memory on the PC and on the Pi.
- Failover times: hub killed before a wake, during a session, during a song.

## 8. Phases

Each phase ends with a working build in both modes and is committed
separately, with what was measured.

| Phase | Board | Hub | Done when |
|---|---|---|---|
| 0. Baseline | Nothing new: record the numbers from section 7 in direct mode | | Numbers written down |
| 1. Link and dashboard | `HubLink`, hello, heartbeat, state push; settings and token | Board endpoint, dashboard served with the `/api/*` proxy and state fan-out | The dashboard works through the hub with two browsers open |
| 2. Voice | `HubAgent` (PCM up, Opus down), backend choice per session, `auto` failover | Gemini session manager, Opus encode, tools, MCP | Sessions through the hub on the PC; failover to direct tested |
| 3. Music control | Control mode in `MusicPlaybackService`, queue copy, `music_*` messages | Music controller, satellites move to the hub | Play, skip, seek, voice "play X", hub dropped mid-song |
| 4. Opus uplink | Opus encoder, format negotiation, uplink settings | Opus decode | CPU measured; `auto` picks sensibly |
| 5. Pi | | Built and run on the Pi Zero 2W | Section 7 measured on the Pi |

## 9. Risks

- **Two backends reacting to the same session state.** Mistakes show up as
  two connections at once or none. The backend choice must be one variable,
  set before the session leaves `Idle`, and each backend checks it.
- **Unencrypted audio on the LAN.** `ws://` keeps TLS off the board, but
  anyone on the home network can listen. Acceptable for a home LAN; `wss://`
  can be added later if needed.
- **Hub is one more always-on thing.** STAR was retired partly for this; the
  direct fallback is why hub mode stays optional.
- **Stream URLs through the hub.** They are signed and IP-bound: the hub
  must never log them (only the host), same rule as on the board.
- **Music control split.** Local actions on the board while the hub is
  deciding (a key press during `music_play`) need a clear owner: the hub
  decides, the board only reports.

## 10. Open questions

- Repository: grow nexus-orbit into the hub, or start `nexus-hub` and move
  the coordinator into it?
- Should a satellite's music move back to the board when the hub drops?
- Several boards (rooms): one Gemini session per board, or shared? Not
  needed for the first version.
