# media_player

The music engine: Invidious search and stream resolution, Opus decoding
(WebM and Ogg), seeking, the queue with autoplay, the SD card cache and the
track library.

## Layers

```
MusicPlaybackService   queue, history, autoplay, commands (media_worker task)
        │
NexusPlayer            one track: local file or stream, pause, seek, end
        │
StorageManager / StreamManager   bytes from the card / from HTTP
        │
AudioEngine            decode task (OpusEngine, core 0, internal stack)
        │  IAudioDecoder: WebMOpusDecoder | OggOpusDecoderStrategy
        ▼
MEDIA_RX_BUF → speaker mixer (audio_core)
```

## What's here

- **`MusicPlaybackService`** — the entry point for everything that plays:
  voice tools, `/api/music/*`, `/api/ws`, keys. Commands go through a queue
  to the `media_worker` task; search, stream resolution and thumbnails run
  on `media_aux` (both with PSRAM stacks). It keeps the queue and history,
  refills the queue with recommendations when autoplay is on
  (`QUEUE_LOW_WATERMARK`), and prefetches the next stream URL. Ways to
  start a track: `play(query)`, `playTrack`, `playDirect(track, url)`,
  `playLocal(id)` (library), `playFile(track, path)` (a file outside the
  library, e.g. a recording: track id `file:<n>`, kept out of the library,
  history and autoplay, stops at the end).
- **`NexusPlayer`** — plays one track. A song saved on the card plays from
  the file; otherwise it streams, saving to the card at the same time when
  caching is on and the track is at most `MAX_CACHE_DURATION_MS` long. It
  owns the playback state in sysdb (`media.state`, `position_ms`,
  `duration_ms`, `seekable`); fills in a missing length from the WebM
  header or the stream URL's `dur`; defers a play or resume while an
  assistant session is active; and has an alarm owner mode, in which an
  alarm song preempts music and the music is restored afterwards.
- **Seeking** — `NexusPlayer::seekTo()` stops the decoder, moves the
  reader and restarts decoding at the target:
  - WebM files: the Cues index in the first few KB (`WebmSeek`) gives the
    cluster; the decoder skips blocks to the exact time.
  - Streams: the same index read from the stream's head, then a new HTTP
    range request from that cluster.
  - Ogg files (recordings): no index, so the reader starts at a byte
    estimate and `OggSeek` finds the page holding the target; the decoder
    gets the header pages again, renumbered, and drops frames to the exact
    time.
- **`StorageManager`** — the card side: cached songs in
  `/sdcard/music/<videoId>.webm` (also `.opus`/`.ogg`), written as `.tmp`
  and renamed when complete; a reader task feeding the decoder; files
  opened by full path for `playFile`.
- **`StreamManager` / `HttpClientStream`** — HTTPS download of a stream,
  with a `Range` header on every request (YouTube's CDN throttles requests
  without one) and restarts at a byte offset for seeks. Redirects are
  followed (googlevideo sends one when the URL was signed for another
  address, e.g. a resolver behind a VPN) and the target is remembered for
  the track. A connection lost before the end (a pause of a few minutes
  lets the server close it) is reopened at the byte where it stopped; a
  403 renews the URL through `MusicPlaybackService` (on `media_aux`) and
  continues if it is the same file. Stream URLs are kept in RAM only.
- **`InvidiousClient` / `InvidiousInstanceResolver`** — search and stream
  resolution against Invidious instances, with health checks and failover;
  `setCustomInstance()` pins one.
- **`AudioEngine`** + **`AudioDecoderFactory`** — the decode task. The
  format is sniffed from the first bytes (EBML → WebM, `OggS` → Ogg). The
  decoded audio is downmixed to mono and resampled to the mixer rate.
- **`CatalogDB`** — the library: one packed `TrackRecord` per saved song
  (title, artist, length, play count, thumbnail) in
  `/sdcard/music/catalog.db`. The dashboard's library reads that file
  directly. It is due to move to `music.ndb` (nexus_db step 5).

## Tests

Host tests (`host_tests/`): `test_webm_seek.py` (Cues, cluster search,
block timecodes, Duration) and `test_ogg_seek.py` (pages, header
renumbering, the seek scan). The rest needs the device.

## Limits

See `docs/known-issues.md`: stream seeks take a few seconds, seeking a song
while it is being saved stops the saving, and resuming mid-track after an
assistant session or alarm hasn't been tested on the device.

## Depends on

`core_sysdb`, `audio_core`, `sd_storage`, `esp_http_client`, `mbedtls`,
`micro-opus` — see `CMakeLists.txt`.
