# Known issues and limitations

What doesn't work, or works with a catch, as of 2026-10-03. Each entry
gives the symptom, the cause if known, and a workaround if there is one.
Fix something here, or find something new: update this file in the same
commit.

## Bugs

### Stream seeks take about 3 seconds
Seeking in a song that is streaming (not saved on the card) resumes about
3 s later; seeks in saved songs and recordings take well under a second.
**Cause:** a seek reconnects (TLS, ~1.1 s with certificate checks; ~0.6 s
before they were turned on, cause of the difference not yet measured) and
downloads from the nearest
index point, up to 10 s of audio before the target, at ~70 KB/s. (A
redirect to a nearby cache, which googlevideo sends when the resolver runs
behind a VPN, is followed once per track and then remembered.) The
download is slow because of the router issue below ("Internet downloads
are capped at ~85 KB/s"); a bigger TCP window measured no difference.
**Options:** fix the link, or start decoding mid-cluster.

### Internet downloads were capped at ~85 KB/s on the home router (fixed)
On the home router (an ISP-provided Nokia gateway, 2.4 GHz) most internet downloads reach the
board at 60-85 KB/s, against 0.4-1.4 MB/s for the same data from the LAN or
through another access point. Gemini's replies need ~64 KB/s, so they have
almost no margin. **Cause** (a promiscuous-mode radio capture, since removed): the
ISP marks each internet connection with an IP precedence (0, 1 or 5,
varying per connection), and the router maps it to a Wi-Fi traffic class:
best effort arrives as TID 1, marked connections as TID 2, 6 or 7. The
board keeps Block Ack (aggregation) sessions for TIDs 0 and 1 only; the
router's ADDBA requests for the other TIDs get no session, and the router
then sends those frames one at a time at 1 Mbps. After TID 2 traffic it
also keeps TID 1 at 1 Mbps until the board reconnects. A connection that
arrives unmarked runs at 0.4-0.8 MB/s. Disabling 802.11b rates, RX buffer
counts and the BA window made no difference. **Workarounds:** another
access point for the board (a laptop hotspot measured 1.2 MB/s), or a relay
on the LAN that re-sends the traffic unmarked (1.4 MB/s); reconnecting
Wi-Fi clears the stuck state. Reported to Espressif: esp-idf issue #19168.
Voice through a hub on the LAN (`voice_backend` hub or auto,
`docs/plans/hub-relay-design.md`) avoids it for Gemini: no underruns in the
first sessions measured.
**Fix (2026-10-27):** the station connects as 802.11b/g by default
(`/api/system/wifi-mode`, `"bgn"` brings 802.11n back). Without HT there
is no Block Ack for the router to set up, and it uses its normal rates.
Measured on the board, same router, after a reboot each: internet plain
HTTP 30-42 → 808-1212 KB/s, HTTPS 24-58 → 292-299 KB/s (TLS CPU-bound),
longest receive pause 0.5-2.4 s → under 0.3 s; LAN 1.2-1.4 → 0.9-1.0 MB/s.

### The hub link dropped on a mic write
The board's link to the hub dropped twice in the hub voice tests: a mic
write waited over a second (`transport_poll_write(0)`) and
esp_websocket_client closes the connection on a write timeout, ending the
session (the link comes back 5 s later). The second time was during a
session over a song download (2026-10-03). The mic write now waits up to
3 s, and writes over 300 ms are logged (`Slow mic write to the hub`); in
four sessions started during fresh song downloads none was slow. If it
comes back, the log line gives the stall's length.

### Low internal RAM after a direct session over music
A cold direct Gemini session (`voice_backend` direct) over music took the
lowest free internal RAM since boot from 22 KB to 12-13 KB, twice. Free RAM
dropped ~13 KB when the TLS connection opened and stayed there; the low
came at the session's end, while the connection was kept open
(`keepalive_s`, ~16 KB of TLS) and the board reopened the mic's DMA and
resumed the music. A session through the hub (no TLS on the board) over
the same music left the low at 22 KB. Likely the same as the 7.5 KB low
seen once on 2026-10-02. **Options:** close the kept connection when music
resumes, or use the hub.

### A rejected stream still shows as playing
When googlevideo refuses a stream (HTTP 403, also after the URL is resolved
again), the player gives up but the status stays `STREAMING` at position 0
until the next command. **Cause:** the stream task's failure isn't passed
back to the player state. **Workaround:** skip to the next song.

### Seeking a song while it is being saved stops the saving
The song keeps playing from the new position but isn't saved this time;
it is saved the next time it plays from the start. **Cause:** the card
reader that follows the download can't seek, so the player switches to
plain streaming.

### AFE "ring full" warnings
For about a second when a stream starts, and while a file is opened for
saving, the log shows the wake-word front end's input ring overflowing:
some mic audio is dropped, so wake-word detection can miss in that
second. **Cause:** not isolated; the AFE feed task falls behind while
core 0 is busy with TLS and SD work. Needs measuring before fixing.

### A small memory leak on every play and stop
About 95 B of internal RAM and 0.5-1.5 KB of PSRAM per track played and
stopped (ten plays lost ~950 B internal, ~15 KB PSRAM). It predates the
seek work. With ~27 KB of internal RAM free while playing, it matters
after a few hundred tracks without a restart. **Next step:** a heap trace
over many plays.

### Reconnecting after a long pause is tested with a forced drop only
A paused stream's idle connection is closed by the server after a few
minutes; on resume the stream reopens at the byte where it stopped, and
renews the URL if it has expired (403). Tested on the device by dropping
the connection on purpose and with a rejected URL; a real pause of several
minutes (and one past the URL's ~6 h expiry) hasn't been run since.

### Resuming mid-track is untested after an alarm
Starting a song part-way after an alarm rang over music was tested on the
PC and with a temporary hook, not end to end on the device. Starting part
way when the hub moves the music (and a play deferred until a session
ends) was tested on the device on 2026-10-03.

### Recordings renamed through the file API
A recording renamed or deleted with the generic `/api/files` routes (not
`/api/recordings`) keeps its old entry in the recordings list until the
next boot, when the card check repairs it. **Workaround:** use the
Recordings page or `/api/recordings/*`.

## Limitations

- **One dashboard at a time.** `/api/ws` takes one client; a new
  connection takes over from the old one. The REST API has no such limit.
- **Recordings stop at 10 minutes.** Stereo recordings drop to 16 kHz if
  music is playing when they start (CPU).
- **The device plays Ogg Opus and WebM Opus only.** WAV recordings play in
  the browser but not on the device.
- **Music comes from Invidious.** Search and playback depend on a working
  Invidious instance; the resolver fails over between instances, and one
  can be pinned.
- **Library `.ogg` files are WebM.** Files named `.ogg` in the music folder
  are WebM inside; the player sniffs the format, so they play normally.
- **Google Search needs a 2.5 Live model.** Search (`web_search`, on by
  default) works on the 2.5 native-audio models. Gemini 3.x Live models on a
  free-tier key close the setup with a quota error; the device then
  reconnects without search, so on those models the assistant has none.
- **Barge-in (talking over a reply, `barge_in` setting) is experimental.**
  The first reply after boot keeps the mic gated, because echo cancellation
  hasn't adapted yet (echo around -30 dBFS against -50 later). An interrupt
  reconnects and resumes the conversation, which takes about 4 s, because
  Gemini sends a reply faster than real time and the reply still queued
  in the TCP window would otherwise play out first (8-20 s).
- **TLS certificate dates aren't checked.** Chains and hostnames are
  verified against the CA bundle, but `CONFIG_MBEDTLS_HAVE_TIME_DATE` is off:
  turning it on makes every connection fail until the clock syncs after
  boot, so the boot order would need checking first.
- **Log buffer.** `GET /api/logs` keeps about 80 lines, so boot messages
  scroll out; the serial console has everything.
- **Serial port.** Only one program may open it; a second one puts the
  chip in a reset loop.
- **Stop clears the queue.** The stop command (dashboard button, voice)
  also empties the queue; pause keeps it.
- **MCP HTTP security policy and payload limit.** Plain HTTP connections to
  an MCP server are restricted to private RFC 1918 LAN addresses (10.x,
  192.168.x, 172.16-31.x, 127.x, localhost, *.local). Public endpoints must
  use HTTPS. Total imported tool declarations are capped at 24 KB to protect
  PSRAM and avoid Gemini handshake rejections.
