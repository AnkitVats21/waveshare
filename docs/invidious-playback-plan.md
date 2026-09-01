# Invidious Playback Migration Plan

## Goal

Remove the dependency on MQTT/MPV for resolving and starting online music playback. Gemini media commands should resolve a YouTube/Invidious result locally on the ESP32 and feed the resulting Opus stream URL into the existing `NexusPlayer` pipeline.

## Architectural decision

Keep the existing audio pipeline intact:

```text
Gemini Live
   |
   v
MediaCommandHandler
   |
   v
MusicPlaybackService
   | \
   |  `-> InvidiousClient
   |          |
   |          `-> search -> videoId -> Opus URL
   |
   v
NexusPlayer
   |
   v
StreamManager -> HttpClientStream -> StorageManager -> AudioEngine -> Speaker
```

MQTT remains available for unrelated/delegated commands, but it is removed from the online music resolution path.

## Components

### 1. InvidiousClient

Responsibilities:

- URL-encode search queries.
- Search an Invidious instance for video results.
- Return a small `InvidiousTrack` metadata object.
- Resolve `adaptiveFormats` to an audio-only Opus URL.
- Own no playback state and have no dependency on `NexusPlayer`, MQTT, buffers, or FreeRTOS playback tasks.

### 2. MusicPlaybackService

Responsibilities:

- Provide the application-level music API (`play`, `playNext`, `pause`, `resume`, `stop`).
- Orchestrate `InvidiousClient` and `NexusPlayer`.
- Keep track of the current resolved item.
- Remain independent from MQTT.

### 3. NexusPlayer / existing pipeline

No redesign is required. `NexusPlayer::play(songId, downloadUrl)` remains the hand-off point. `StreamManager` continues to consume the resolved URL and push network data into the existing buffers.

## Migration phases

### Phase 1 — Local single-track playback

- Add `InvidiousClient`.
- Add `MusicPlaybackService`.
- Route Gemini `play(query)` directly to the service.
- Route `play_next(query)` locally during the migration.
- Route pause/resume/stop directly to `NexusPlayer`.
- Keep volume/mute/autoplay/next/previous MQTT delegation temporarily where their local queue semantics have not yet been migrated.

### Phase 2 — Local queue

Introduce `PlaylistManager` above `NexusPlayer`.

- `play(query)` clears/replaces the queue.
- `play_next(query)` resolves and inserts an item at the front of the pending queue.
- `next()` advances locally.
- `previous()` uses local playback history.
- `stop()` clears queue and playback state.

### Phase 3 — Local autoplay

Replace the playback-finished MQTT `next` publication in `NexusPlayer` with a local playback-finished notification/callback to `MusicPlaybackService`.

The service then advances the local queue and starts the next item without leaving the device.

### Phase 4 — Pre-resolution

Resolve the next queued item in a background task while the current item is playing. Store the resolved URL with the queue item so the next track can begin without waiting for search/URL resolution after EOF.

### Phase 5 — Configuration and resilience

- Move the Invidious instance from a compile-time constant to Kconfig/NVS configuration.
- Support instance fallback/rotation if required.
- Add timeouts and useful error states.
- Avoid logging complete signed stream URLs in production logs.
- Add retry behavior for transient Invidious failures.

## Important constraints

- Do not make `NexusPlayer` aware of Invidious.
- Do not make `StreamManager` aware of Invidious.
- Do not duplicate HTTP streaming logic already present in `HttpClientStream`.
- Keep MQTT for functionality that still genuinely requires it.
- Do not assume every Invidious instance exposes identical format metadata; validate the returned MIME/type and URL before playback.
- The selected format must remain compatible with the existing Ogg/Opus decoder in `AudioEngine`.

## Current implementation on this branch

Implemented now:

- `InvidiousClient.h/.cpp`
- `MusicPlaybackService.h/.cpp`
- Gemini PLAY and PLAY_NEXT no longer publish to `mpv/command`.
- PAUSE, RESUME and STOP are handled locally.
- Existing streaming/cache/decode pipeline remains unchanged.
- New sources are included in `main/CMakeLists.txt`.

Not yet migrated:

- Local next/previous queue/history.
- Playback-finished notification and local autoplay.
- Background pre-resolution.
- Configurable Invidious instance.

## Validation checklist

1. Device has Wi-Fi connectivity.
2. Gemini `play` produces a plain text query.
3. Invidious search returns a video ID.
4. Video metadata contains a usable audio-only Opus format.
5. `NexusPlayer` receives the video ID and resolved URL.
6. Existing `StreamManager` receives network bytes normally.
7. `AudioEngine` decodes the Ogg/Opus stream and produces 32 kHz mono PCM.
8. Cache behavior remains unchanged.
9. Assistant interruption/pause/resume behavior remains unchanged.
10. No `mpv/command` publication occurs for PLAY/PLAY_NEXT/PAUSE/RESUME/STOP.
