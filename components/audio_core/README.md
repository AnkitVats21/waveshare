# audio_core

Real-time audio I/O, mixing/orchestration priority, alert tones, and local
wake-word detection. Everything here runs time-critical work pinned to
Core 1 (`CORE_AUDIO`).

## What's here

- **`AudioOrchestrator`** — the focus/ducking coordinator. Owns a priority
  hierarchy (Voice Assistant `GEMINI_SPEAKING` > Alert Tones `ALERT` >
  Music Playback `NEXUS_STREAMING` > Idle) and pauses/resumes lower-priority
  audio sources automatically when a higher-priority one becomes active.
  `media_player`'s `NexusPlayer` and this component's own `AlertPlayer`
  both register with it rather than writing to the speaker directly.
- **`SpeakerPlayback`** — the I2S DMA writer task. Mixes whatever the
  orchestrator currently has active and writes it out with jitter control.
- **`MicCapture`** — the I2S DMA reader task feeding both the wake-word
  engine and the Gemini Live uplink (`GeminiAudioPump`, in `gemini_live`).
- **`WakeWordEngine`** — wraps ESP-SR WakeNet + AFE (AEC/VAD). Gated by
  `CONFIG_WAVESHARE_WAKEWORD_ENABLE` (off by default); when enabled it
  notifies `AssistantService` (in `gemini_live`) on detection.
- **`AlertPlayer`** — chime/tone playback (used for the wake/confirm chime
  and other short system sounds).
- **`Resampler`** — fixed-point linear resampler; primarily used to
  convert Gemini Live's 24kHz downlink audio to the board's native 32kHz
  I2S output rate on the fly.

## Header layout

Each public header exists at three include paths that all resolve to the
same content: `include/<Name>.h`, `include/audio_core/<Name>.h` (the
canonical one), and callers elsewhere in the firmware include it as
`"audio_core/<Name>.h"`. The bare `include/<Name>.h` files are one-line
forwarding shims kept for compatibility — prefer `audio_core/<Name>.h` in
new code.

## Core affinity

- **Core 1**: `SpeakerPlayback`, `MicCapture`, wake-word `ww_feed`/`ww_detect`
  tasks. Isolated here specifically so Core 0's network/TLS bursts can't
  starve the microphone ring buffer.
- Depends on `core_sysdb` (for `EmbeddedSysDb`/`ReactorTask`), `esp_codec_dev`,
  `esp-sr`, and `esp_driver_i2s` — see `CMakeLists.txt`.

## Gotchas

- `AudioOrchestrator` ducking is cooperative: a source must call into it
  rather than writing to `SpeakerPlayback` directly, or it'll bypass the
  priority hierarchy entirely and cause overlapping audio.
- `onStateChanged()` overrides anywhere in this component's `ReactorTask`
  subclasses must stay non-blocking (see `core_sysdb`'s `ReactorTask`
  contract) — heavy I2S/HAL work belongs in `run()`.
