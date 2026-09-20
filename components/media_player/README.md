# media_player

The streaming music engine: Invidious search/resolution, Opus decoding,
gapless prefetch/autoplay, and SD card caching + a binary on-device track
catalog.

## What's here

- **`NexusPlayer`** — high-level player interface; owns playback state and
  drives `AudioOrchestrator` (in `audio_core`) for output.
- **`MusicPlaybackService`** — queue, prefetch, and autoplay manager.
  Monitors queue depth with a low-watermark algorithm
  (`QUEUE_LOW_WATERMARK = 2`) and proactively resolves/prefetches upcoming
  tracks in the background so transitions are gapless. Background prefetch
  (`bg_prefetch`) and replenishment (`bg_replenish`) tasks run with
  `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` stacks to keep internal SRAM free.
- **`InvidiousClient` / `InvidiousInstanceResolver`** — search + stream
  resolution against Invidious-compatible instances. The resolver
  maintains a rotating list of public instances with health checks
  (`testInstance()`) and automatic failover (`markInstanceFailed()`); call
  `setCustomInstance()` to pin a specific host. There's no Kconfig option
  for this — it's entirely runtime-resolved.
- **`AudioDecoderFactory`** + **`OggOpusDecoderStrategy`** / **`WebMOpusDecoder`**
  — strategy-based Opus decoding for Ogg and WebM containers (`micro-opus`).
- **`StorageManager`** — SD card stream caching. When SysDb's
  `media.cache_downloads` is enabled, downloaded audio streams are saved
  to `/sdcard/cache/<videoId>.opus` for offline replay.
- **`CatalogDB`** — the real, active on-device track catalog: a compact
  packed binary record format (`TrackRecord`, `SeekEntry`) tracking
  cached files, seek tables, thumbnails, and pin/eviction flags. This is
  what backs the web dashboard's SD Library view.
- **`StreamManager` / `HttpClientStream`** — HTTP stream lifecycle and
  buffering for the audio pipeline.

## Dead code warning: `MusicLibraryManager`

`src/MusicLibraryManager.cpp` and `include/media_player/MusicLibraryManager.h`
exist on disk but **`MusicLibraryManager.cpp` is not in `CMakeLists.txt`'s
`SRCS` list and nothing outside the file itself references the class**.
It looks like an earlier library-index implementation that `CatalogDB`
superseded. Don't build on it without first checking whether it's still
meant to be there — it may be safe to delete, but that wasn't confirmed
as part of this doc pass.

## Depends on

`core_sysdb` (state/schema), `audio_core` (`AudioOrchestrator` output),
`esp_http_client`, `mbedtls`, `cjson` — see `CMakeLists.txt`.
