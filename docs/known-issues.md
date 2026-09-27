# Known issues and limitations

What doesn't work, or works with a catch, as of 2026-09-27. Each entry
gives the symptom, the cause if known, and a workaround if there is one.
Fix something here, or find something new: update this file in the same
commit.

## Bugs

### Stream seeks take about 4 seconds
Seeking in a song that is streaming (not saved on the card) resumes 3-4 s
later; seeks in saved songs and recordings take well under a second.
**Cause:** a seek reconnects (TLS, ~0.6 s) and downloads from the nearest
index point, up to 10 s of audio before the target, at ~70 KB/s. When the
resolver runs behind a VPN (WARP), every connect also follows a 302 to a
nearby cache (~0.6 s more), because the URL is signed for the VPN's
address. The
download is slow because the TCP receive window is small
(`CONFIG_LWIP_TCP_WND_DEFAULT` 5760), which is small because internal RAM is
short. **Options:** a bigger window if RAM allows, or start decoding
mid-cluster.

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

### Resuming mid-track is untested on the device
Starting a song or stream part-way (after an alarm rings over music, or a
play deferred until an assistant session ends) uses code paths that were
tested on the PC and with a temporary hook, but not end to end on the
device.

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
- **Gemini 3.x Live and Google Search.** Gemini 3.x Live models reject the
  `googleSearch` tool with the current key and end the session (Live 2.5
  native-audio accepts it), so search isn't offered to the assistant.
- **Log buffer.** `GET /api/logs` keeps about 80 lines, so boot messages
  scroll out; the serial console has everything.
- **Serial port.** Only one program may open it; a second one puts the
  chip in a reset loop.
- **Stop clears the queue.** The stop command (dashboard button, voice)
  also empties the queue; pause keeps it.
