# Plan: Hub mode (voice relay and music control on a LAN hub)

Status: **phase 3 built** (edges and roles, music on the hub, section 14);
phases 4 (Opus uplink) and 5 (the Pi) next
Last updated: 2026-10-03

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

Since phase 3 the hub is not only the board's hub: the board is one *edge*
among others (Orbit on a PC or Pi, a phone later), all speaking one protocol
(section 13). The board keeps its full standalone mode.

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
- The hub is written in Go, as a second program in the nexus-orbit repository
  (`cmd/nexus-hub` beside `cmd/orbit`), sharing its message definitions
  (`pkg/protocol`) and connection code (`pkg/transport`).

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
| hub | `rejected` | reason (bad token); the hub closes the socket, and the board tries again after 5 minutes or a settings change |
| both | WebSocket ping / pong | every 5 s; no answer for 10 s marks the link down (a closed socket marks it down at once). Protocol-level pings, not JSON messages: both WebSocket libraries answer them on their own |

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
| board | state (no `type`) | the same JSON `/api/ws` pushes, on change (at most every 200 ms) and every second |
| board | `transcript` | the same transcript messages as `/api/ws`; the board always sends them to the hub, and the hub keeps a copy for browsers that subscribe |
| hub | `{"cmd": ...}` (no `type`) | a browser's command, forwarded as the browser sent it and handled like an `/api/ws` command |

## 5. Hub side

A Go program in the nexus-orbit repository (`cmd/nexus-hub`), one process.
Shared with the satellite (`cmd/orbit`): `pkg/protocol` (so the satellite and
board music messages are defined once) and `pkg/transport`. A Pi with a
speaker can run both. nexus-orbit's `coordinator` is the satellite's player
logic, not a queue owner: the hub's music controller is new code, a port of
what `MusicPlaybackService` does on the board.

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
- **Orbit satellites.** The satellite endpoint (today `OrbitChannel` on the
  board), on the hub.
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
| Hub drops with satellites playing | Satellites stop. The music is not moved back to the board: without the hub nobody knows the satellite's position or queue. The board keeps a queue copy only while it is the output itself, and is in local mode, so a new "play" works at once. |

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

### Baseline, direct mode (phase 0, 2026-10-02)

Firmware 54b21f9 on the home Wi-Fi, five sessions from `tools/voice_test`,
no music playing.

| Measure | Value |
|---|---|
| Internal RAM free, idle, no connection | 61.2 KB (largest block 43 KB) |
| Same, connection parked after a session | 45.0 KB (the TLS connection holds ~16 KB) |
| Lowest seen during a session / since boot | 37.3 KB / 34.7 KB |
| Wake to listening, cold (connect + TLS + setup) | 2.0 s; setupComplete at 2.8 s |
| Wake to listening, warm (parked connection) | 27-32 ms |
| Reply delivery rate | 0.74-0.86× real time, largest gap between frames 356-787 ms, board queue blocked 0 ms |
| Underruns per reply | 4-29 (20-28% of the reply time starved) |

The slow delivery is the router issue in `docs/known-issues.md`: the Nokia
access point sends ISP-marked flows at 1 Mbps, and Gemini's is one of them.
The board is not the limit (its queue never blocks). Over the LAN the same
board receives 1.4+ MB/s, so a hub on the LAN should remove these underruns.
This is the main number for phase 2 to beat.

Not measured yet: a session over music (the music output was a satellite),
and wake to the first reply audio (no log line marks the first frame; to
add with phase 1).

## 8. Phases

Each phase ends with a working build in both modes and is committed
separately, with what was measured.

| Phase | Board | Hub | Done when |
|---|---|---|---|
| 0. Baseline | Nothing new: record the numbers from section 7 in direct mode | | Numbers written down |
| 1. Link and dashboard | `HubLink`, hello, heartbeat, state push; settings and token | Board endpoint, dashboard served with the `/api/*` proxy and state fan-out | The dashboard works through the hub with two browsers open |
| 2. Voice | `HubAgent` (PCM up, Opus down), backend choice per session, `auto` failover | Gemini session manager, Opus encode, tools, MCP | Sessions through the hub on the PC; failover to direct tested |
| 3. Music control | Roles in the hello; `MusicPlaybackService` as player + controller with a queue copy (section 13) | Music controller and music tools, edges with roles, Orbit on the hub, music cache | Play, skip, seek, voice "play X" through either voice path, Orbit as an output, hub dropped mid-song |
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

## 10. Phase 1 as built (2026-10-02)

- **Board:** `main/services/hub/HubLink` (`CONFIG_WAVESHARE_VOICE_HUB`,
  default off) and a relay sink in `ControlChannel`: the same state pushes and
  transcript go to the hub, and the hub's `{"cmd"}` frames are handled like
  `/api/ws` commands. `GET /api/hub` shows the link; `POST /api/hub` sets
  `url` (setting `hub_url`) and `token` (NVS, write-only).
- **Hub:** `cmd/nexus-hub` and `pkg/hub` in nexus-orbit. Board tokens are
  read from `~/.config/nexus/hub_tokens`, one per line. Browsers are closed
  while no board is connected (close code 4003) and reconnect on their own.
  `/api/*` is forwarded to the board's address as the hub sees it plus the
  `http_port` from the hello.
- **mDNS** works from the hub's side (advertised on LAN interfaces only;
  Docker and VPN bridges are skipped, since their addresses are no use to a
  board). On the home router it does not cross from the 5 GHz band (PC) to
  the 2.4 GHz band (board), in either direction (`nexus.local` never
  resolved from the PC either), so the board there uses `hub_url`.
- **Measured** on the board, home Wi-Fi:
  - Internal RAM: 55.4 KB free when idle with the link up, against 61.2 KB
    without it in the baseline. The cost is the client task's stack (3.5 KB,
    1.7 KB used) and the socket. The client's two buffers are made larger than
    4096 bytes so that malloc puts them in PSRAM; at 2048 bytes they took
    another 4 KB of internal RAM.
  - The link comes up 8 s after boot.
  - A command from one browser shows up in another browser's state 121 ms
    later.
  - REST calls through the hub take 35-100 ms, the same as direct.

## 11. Phase 2 as built (2026-10-02)

Changes from section 4:

- **PCM both ways.** Reply audio comes down as 24 kHz PCM, not Opus: the LAN
  carries 48 KB/s easily and isn't marked by the ISP, and an Opus decoder on
  the board would need an internal-RAM stack. The welcome says
  `voice: {"up": "pcm", "down": "pcm", "down_rate": 24000}`, or `false` when
  the hub has no Gemini key. Binary frames: byte 0 channel, byte 1 flags,
  then raw 16-bit little-endian samples (one packet per frame; Opus, if
  added, would use the length-prefixed packets).
- **The board sends its whole setup.** `session_start` carries
  `{setup, web_search, keepalive_s, resume_min, text?}`; `setup` is what
  `GeminiProtocol::buildSessionSetup` builds for a direct session (settings,
  prompt, memory file, VAD, the board's skill declarations), ~13 KB. No
  skills hash or `skills` message. The hub adds Google Search (2.5 models,
  with the same prompt line and without `get_weather`), the MCP tools and the
  resumption handle.
- **Messages.** Hub to board: `session_ready {resumed}`, `transcription
  {role, text}`, `tool_call {id, name, args}`, `reply_owed` (an MCP tool is
  running), `turn_complete` and `interrupted` (in order after the audio),
  `session_error {reason}`. Board to hub: `session_start`, `text_turn`,
  `tool_response {id, result}`, `interrupt`, `session_end`. The board keeps
  its own "reply owed" logic, fed by transcriptions, tool calls and
  `reply_owed`.
- **Pacing.** The hub sends 60 ms frames at most 1.5 s ahead of the board's
  playback (`VOICE_RX_BUF` holds ~5 s), so the link never blocks behind
  audio. Audio is written into `VOICE_RX_BUF` on the link's client task.
- **Backend choice.** Made at the wake (`AudioService::onWakeWord`), which
  asks for the connection in the same step; `ws_state` is reset when the
  backend changes. `hub` with the hub away fails at Connecting with the
  error chime.
- **On the hub:** resumption, goAway handoff at a turn end (mic held up to
  4 s meanwhile), the search quota fallback, the kept connection after a
  session, MCP tools (`--mcp-url`, token file; same filters and limits as
  the board). An interrupt drops the queued reply and the rest of it; the
  hub reads Gemini at full speed, so no reconnect is needed.

Measured on the board, hub on the PC, gemini-3.1-flash-live, 8 turns:

| Measure | Direct (phase 0) | Hub |
|---|---|---|
| Underruns per reply | 4-29 | 0 |
| Delivery to the board | 0.66-0.86x real time, gaps up to 888 ms | 1.3-3.7x (paced), largest gap 71-212 ms |
| Gemini to the sender | (the board) | 4-7x real time |
| Wake to listening, cold / kept connection | 2.0 s / 27-32 ms | 1.0-1.5 s / 61 ms |
| Internal RAM idle / lowest | 61.2 KB, 45.0 KB with a kept connection / 34.7 KB | 54.3 KB / 38.5 KB |

Failover tested: hub away with `auto` goes direct; the hub back while a
direct connection is still kept goes to the hub; the hub killed mid-session
ends it with the error chime; `hub` with the hub away gives the error chime.

Not done yet: a session over music, MCP tools through the hub (the hub's
token file must hold the deployed server's token), barge-in through the
hub, a goAway handoff on the device, and the one link drop in
`docs/known-issues.md`.

## 12. Settled after review (2026-10-02)

- **Repository:** the hub is a second program in nexus-orbit, not a new
  repository; the shared protocol package keeps the satellite and board
  messages from drifting apart. The repository has no remote yet, so a rename
  later costs nothing.
- **Satellite music when the hub drops:** it stops; it does not move to the
  board (section 6).
- **Several boards:** one Gemini session per board (rooms are separate
  conversations). `hello` carries the device id from the start; the first
  version assumes one board.

## 13. Edges and roles (phase 3 plan, 2026-10-02)

The hub is the brain; every device is an edge. An edge does audio in and out
(wake word, mic, speaker), plays music and sends what the person does on it.
The ESP board is one edge; Orbit on Linux, Windows or a Pi is another; an
Android app could be a third. They speak the same protocol and differ only in
how they implement it. Logic that more than one kind of edge would need goes
in the hub.

The ESP board keeps everything it does today in direct mode (Gemini, its
skills, MCP, the queue, the card cache, alarms, satellites on `/api/orbit/ws`).
Hub support stays a build option and a setting, and the board carries on
alone when the hub goes away. Other edges need no standalone mode.

The protocol is written down as its own document, `docs/PROTOCOL.md` in the
nexus-orbit repository, from which any new client is built. It replaces
sections 4.1 and 4.3 here. In short:

- **Roles in the hello:** `voice`, `player`, `controller`, `state` (the
  board's state push and HTTP API, board only). A hello without roles is
  read as a phase 2 board.
- **The hub owns the music tools.** In a hub session, `play`, `playback`
  and `music_output` run in the hub's music controller; the board's
  declarations with those names are dropped from the setup it sends. Any
  voice edge gets music without implementing it. (This was section 5; it
  is now in the protocol.) In direct mode the board's own skills run as
  today, and act on the hub's queue through the controller role.
- **One player protocol** for the board and Orbit (`music_play`, pause,
  resume, seek, stop, `music_queue`, `music_url`; `music_report`,
  `music_event`, `music_renew`). Picking the output is picking a player.
- **Queue copy:** a player that declares `queue_copy` (the board) gets the
  queue and carries on without the hub; when the link returns, `music_sync`
  makes its state the hub's.
- **A voice session pauses music wherever it plays:** the edge pauses its
  own music, and the hub pauses another edge's for the session (it learns of
  direct sessions from `voice_activity`).

Steps, each committed and tested on its own:

| Step | Where | What |
|---|---|---|
| 3.1 | hub | `docs/PROTOCOL.md`; edges with roles (several at once, by id); music controller: queue, search and stream URLs through nexus-mcp `music_track` (invidious-daemon as fallback), next track's URL fetched ahead, repeat, autoplay from recommendations; the music tools. Tested against fake edges |
| 3.2 | board | Hello with roles; setting `music_control` (`auto`: the hub when its welcome says `music`, else `local`). In hub mode `MusicPlaybackService` sends keys, dashboard, voice and queue edits as `music_action`, plays what the hub sends, mirrors `music_queue` into its own queue (so the dashboard needs no change), reports position and track ends; local again when the link drops, `music_sync` when it returns. Recordings and alarms stay on the board |
| 3.3 | Orbit | Orbit connects to the hub as a player (and controller for its media keys) with the new messages; its board protocol stays for direct mode |
| 3.4 | hub | Music cache: the hub keeps the songs it has played (size limit, least recently used out) and serves them to players over the LAN; fetches the next song ahead. The board keeps its card cache |
| 3.5 | all | Measure: play, skip and seek delay, gap between songs, hub killed mid-song and on the last song, hub restart; RAM on the board. Write "Phase 3 as built" |

Not in phase 3: a hub-side default setup (prompt, voice) for edges that send
none, and a dashboard that reads `music_state` from the hub directly.

## 14. Phase 3 as built (2026-10-03)

Built as planned in section 13; the protocol is nexus-orbit
`docs/PROTOCOL.md` (proto 2). Changes and additions:

- **Board:** `HubMusic` (`main/services/hub/`) is the player and controller
  side; `MusicPlaybackService::postCommand` is the person's entry point and
  goes to the hub while it runs the music, while the media worker's own
  steps (`dispatch`) stay local. Setting `music_control`: `auto` (default)
  or `local`, in `POST /api/hub {"music"}`. The board's queue is the hub's
  copy, so the dashboard shows it unchanged; music on another edge shows as
  a satellite's in the board's media state.
- **Hub:** a lookup goes through nexus-mcp `music_track`, then
  invidious-daemon (`--invidious-url`). The next song's URL is fetched
  ahead. The music cache (`--cache-dir`, `--cache-max-mb`) serves saved songs
  to every player over the LAN.
- **Orbit:** `--hub ws://<hub>:8380/hub/edge` joins as a player; without it
  Orbit talks to the board's `/api/orbit/ws` as before (direct mode).
- **Sessions and music:** the hub remembers that music was playing when a
  session began and resumes it afterwards wherever it plays by then (moved
  to Orbit during the session, it starts there when the session ends).
- **Fixed along the way:**
  - A play deferred until a session ended lost its song id when music was
    playing. This also hit the direct path for a song saved on the card.
  - The silence timeout counted the connection time; a cold direct
    session (2.3 s) left under a second to speak.
  - The mic write to the hub waits up to 3 s. A 1 s stall during a song
    download had dropped the link.

Measured on the board (hub on the PC, home Wi-Fi):

| Measure | Value |
|---|---|
| Key, dashboard or voice action to the hub | 10-20 ms on the board |
| "Play X" by search | 11-13 s, all in the lookup (invidious-daemon: search 4.6 s, first extraction ~7 s; the same lookup as direct mode) |
| Library song (saved on the card) | 0.5 s |
| Next (URL fetched ahead) | 150 ms |
| A song's end to the next song | 330 ms |
| Seek | 40 ms |
| Voice "next" in a direct session, through the hub | 360 ms |
| A song saved by the hub (3.2 MB) | 228 ms |
| The board streaming a saved song from the hub | opened 0.5 s after `music_play` (plain HTTP, 206) |
| Internal RAM idle / streaming from YouTube (lowest) | 53.8 KB / 22.4 KB |
| A hub session over music | no new low |
| A cold direct session over music | low 12.9 KB (`docs/known-issues.md`) |

Failover tested on the device:
- **Hub killed mid-song:** the song carries on. At its end the board plays
  the next from its copy; the lookup for it now starts when the link drops.
- **Hub back:** `music_sync` makes the board's song and queue the hub's.
  The next song end, reported as seq 0, advances through the hub.
- **`music_control` set to local, then auto:** the board's own queue, then
  sync.
- **Satellite closed:** the hub stops the music and the board shows idle.
- **Hub killed with Orbit playing:** Orbit stops.
- **By voice through the hub:** music moves from the board to Orbit and
  back, at its position.

Not done:
- The dashboard reads the hub's `music_state` (it shows the board's copy).
- A default setup on the hub for edges that send none.
- Orbit's media keys as controller actions.
- The board's dashboard picker (`/api/orbit/target`) acts on the board's
  own satellites, not the hub's.
