# Plan: Desktop-testable `media_player` and `gemini_live`

Status: **proposed** — not started (still open, checked 2026-09-27; the
numbers below are from 2026-09-21)
Owner: unassigned
Last updated: 2026-09-21
Goal confirmed: **desktop testing**, not a second production platform.

---

## 1. The finding that shapes this plan

**This repo already has a working desktop test harness, and it already solves
the hard parts.** `host_tests/` contains 614 lines of ESP-IDF shim headers that
let ESP-coupled code compile and run natively:

```
host_tests/shims/
  esp_log.h  esp_err.h  esp_heap_caps.h  esp_timer.h
  freertos/  FreeRTOS.h  task.h  queue.h  semphr.h  ringbuf.h  idf_additions.h
```

These are not stubs — `task.h` implements FreeRTOS task notifications over
`std::thread` + `condition_variable`, and `ringbuf.h` is a real ring buffer with
borrow/return semantics. On top of them, `EmbeddedSysDb`, `WalRingBuffer`,
`SysDbCodec`, **`BufferManager`**, **`ReactorTask`**, and `Resampler` already
compile and run on the host, exercised by pytest through nanobind bindings.

This matters because in my earlier analysis I named `BufferManager` and
`ReactorTask` as the two most viral blockers to portability. **For testing
purposes they are already solved.** The pattern to follow is the one this repo
established, not a ports-and-adapters refactor.

**Revised recommendation: extend the shim layer, don't restructure the
components.** That is roughly **1.5–2 weeks instead of 4–6**, and it touches
almost no production code — so it cannot regress the firmware.

> A full `core/` + `ports/` + `platform/` extraction is still the right answer
> *if a second production platform is ever planned*. That version is preserved
> in section 8 as a deferred option. It is not justified by desktop testing alone.

---

## 2. What already works

| Piece | Status |
|---|---|
| Host CMake + nanobind + pytest | `host_tests/run_tests.sh` — creates venv, builds, runs |
| FreeRTOS tasks/queues/semaphores/ringbufs on desktop | shimmed, working |
| `heap_caps` PSRAM allocation | shimmed to `malloc` |
| `EmbeddedSysDb`, `ReactorTask`, `BufferManager` | compiling + tested on host |
| Existing suites | `test_resampler`, `test_star_wal`, `test_buffer_manager`, `test_sysdb_concurrency` |

## 3. Gap analysis — what's actually missing

I traced the full include graph of both components against the existing shims.
The gap is smaller than expected:

| Needed | For | Effort |
|---|---|---|
| `esp_http_client.h` shim + programmable fake | `HttpClientStream`, `InvidiousClient`, `InvidiousInstanceResolver`, `MusicPlaybackService` | **M** — the one real piece of work |
| `esp_websocket_client.h` shim + programmable fake | `WssClient` → `GeminiProtocol` | **M** |
| `freertos/event_groups.h` | `AudioEngine` | S |
| `esp_rom_crc.h` (`esp_rom_crc32_le`) | `CatalogDB` | XS — any crc32 |
| `esp_crt_bundle.h` | HTTP/WS TLS setup | XS — empty stub |
| `mbedtls/base64.h` | `GeminiProtocol` | XS — system mbedtls or ~40 LOC |
| ArduinoJson include path | skills parsing, Invidious | XS — header-only, builds on desktop as-is |
| libopus + micro-opus include path | `WebMOpusDecoder`, `OggOpusDecoderStrategy` | S — link system `libopus` |

Notably **not** needed: `AudioOrchestrator.h` includes only
`freertos/FreeRTOS.h` + `semphr.h` + `<vector>` + `<cstdint>` — all already
shimmed. So even `NexusPlayer`'s header graph is reachable without touching
`audio_core`'s hardware layer.

---

## 4. Phased execution

Each phase ends with green tests and does not modify firmware behaviour.

### Phase 1 — Close the shim gap (M, ~2–3 days)
- Add `esp_rom_crc.h`, `esp_crt_bundle.h`, `freertos/event_groups.h`,
  `mbedtls/base64.h` (or link system mbedtls).
- Wire ArduinoJson and system `libopus` into `host_tests/CMakeLists.txt`.
- **Deliverable:** `WebMOpusDecoder`, `OggOpusDecoderStrategy`,
  `AudioDecoderFactory`, `CatalogDB`, and `gemini_skills_generated` all
  *compile* on the host. No tests yet.

### Phase 2 — Decoder + catalog tests (M, ~2 days) ← **the main prize**
- Bind the decoders and `CatalogDB` via nanobind.
- Capture fixtures: a few short `.webm`/`.ogg` payloads, a real Gemini
  tool-call JSON frame, a populated catalog file.
- Tests: decode → assert PCM length/rate/channels; truncated and corrupt
  streams; `CatalogDB` record round-trip, seek tables, CRC rejection, eviction.
- **This is where the 100× loop actually lands.** ~1,150 LOC of the nastiest
  byte-level code becomes editable without flashing.

### Phase 3 — Programmable network fakes (L, ~3 days)
The interesting design work. Rather than a passive stub, make the shim
*scriptable* so production code runs unmodified:

```cpp
// host_tests/shims/esp_http_client.h  (test-facing API)
mock_http_set_response("/api/v1/search", 200, R"([{"videoId":"..."}])");
mock_http_fail_next(ESP_ERR_HTTP_CONNECT);          // force failover path
mock_http_set_chunked("/videoplayback", body, 4096); // exercise partial reads
```

- Same shape for `esp_websocket_client.h`: inject inbound frames, assert
  outbound ones.
- **Deliverable:** `InvidiousInstanceResolver` failover/health-check policy and
  `GeminiProtocol` frame encode/decode become testable with **zero production
  code changes and no network**.

### Phase 4 — Policy tests + targeted seams (M, ~2–3 days)
- Test `MusicPlaybackService` queue/prefetch/autoplay (the low-watermark
  algorithm) and `AssistantService`'s state machine.
- Expect to hit **global-state friction** here (see risks). Where a test can't
  be written cleanly, apply the *smallest* seam that fixes it — usually
  extracting a pure decision function (`nextPrefetchAction(queueDepth, ...)`)
  rather than injecting an interface. Prefer functional-core extraction over
  dependency injection; it is cheaper and reads better.
- `esp_timer` is already shimmed, but consider a settable fake clock so
  backoff/timeout logic is deterministic rather than wall-clock dependent.

### Phase 5 — CI (S, ~0.5 day)
- Add a `host-tests` job to `.github/workflows/` next to the firmware build.
- Runs in seconds, gives PR-level protection on the logic the firmware build
  can only prove *compiles*.

**Total: ~1.5–2 weeks.** Phases 1–2 (~4–5 days) deliver the bulk of the value.

---

## 5. Where shims are *not* enough — honest limits

Be clear-eyed that this approach tests **logic**, not **the system**:

- **You are testing against mock FreeRTOS, not FreeRTOS.** Scheduling,
  preemption, priority inversion, and core affinity behave differently. A race
  that only manifests under real ESP scheduling will not be caught here.
- **No memory-reality checks.** `heap_caps` shims to `malloc`, so PSRAM
  exhaustion, fragmentation, stack overflows, and DMA alignment issues stay
  invisible. Those remain hardware-only concerns.
- **Timing is not real.** Audio underruns, I2S jitter, and network stalls under
  TLS load can't be reproduced.
- **Hardware paths stay untestable** by design: `MicCapture`, `SpeakerPlayback`,
  I2S DMA, the ESP-SR wake word engine, codec-device control.

The mitigation is scope discipline: use host tests for **container parsing,
catalog/seek-table formats, JSON/protocol framing, queue and failover policy,
and state-machine transitions**. Keep hardware and timing verification on-device.
Don't let a green host suite create false confidence about a flash-and-run.

---

## 6. Risks

| Risk | Mitigation |
|---|---|
| **Singleton state leaks between tests.** `EmbeddedSysDb`, `BufferManager`, `NexusPlayer`, `AudioOrchestrator` are all `getInstance()` singletons in one pytest process — test N pollutes test N+1. | Check how existing suites handle this in `conftest.py`; otherwise use `pytest-forked` / `--forked` for process isolation, or add explicit reset hooks. Decide this in Phase 2, before the suite grows. |
| **Shim drift.** Shims silently diverge from real ESP-IDF semantics as IDF is upgraded, giving false green. | Keep shims minimal and comment each deviation. Treat a host-only pass as necessary-not-sufficient; the firmware CI build remains the gate. |
| **Scope creep into a full refactor.** Phase 4 tempts you toward restructuring. | Rule: no seam larger than extracting a pure function. Anything bigger is deferred to section 8. |
| **Fixture bloat.** Binary audio fixtures in git. | Keep them a few KB each — truncated streams are fine for parser tests. |
| **Generated sources.** `gemini_skills_generated.*` is CMake-generated from JSON. | Host CMake must run `generate_gemini_skills.py` too, or the host build tests a stale copy. |

---

## 7. Success criteria

1. `./host_tests/run_tests.sh` builds and runs decoder, `CatalogDB`, Invidious
   failover, and Gemini frame-parsing suites in **under 10 seconds**.
2. A deliberately corrupted WebM fixture and a forced instance failover are both
   covered by tests that fail if the logic regresses.
3. **Zero behavioural changes to firmware.** `idf.py build` output size within
   noise of `d7db2c8`.
4. Host tests run on every PR in GitHub Actions.
5. No new interface/abstraction introduced unless a test was otherwise
   unwritable.

---

## 8. Deferred: full portability refactor

Only justified if a **second production platform** appears (a Linux daemon, Pi
build, or another MCU). Not justified by testing alone. If that changes, the
work is:

- `core/` (pure C++17) + `ports/` + `platform/{esp,posix}` per component.
- ~8 interfaces: `IByteStream`, `IWebSocket`, `IBytePipe` (must preserve
  ringbuf borrow/release or you add a copy per audio chunk), `IAudioSink`,
  `IAudioSource`, `IClock`, `ILogger`, `IAllocator`.
- Invert `EmbeddedSysDb` out via observers (`IPlaybackObserver` already exists;
  add `IAssistantObserver`), with SysDb adapters wired in the ESP app.
- Replace `TaskBase`/`ReactorTask` threading with `std::thread` —
  `esp_pthread_set_cfg()` in IDF v6.0.1 exposes `stack_size`, `prio`,
  `pin_to_core`, and `stack_alloc_caps`, so PSRAM stacks and core pinning
  survive.
- De-singleton via constructor injection; `NexusPlayer` (575 LOC, `ReactorTask`
  + `IAudioFocusObserver` + `DECLARE_BUFFER` globals) is the hardest piece and
  should be done last.
- Estimated 4–6 weeks.

Existing seams to build on if that day comes: `IStorageService`,
`IPlaybackObserver`, `IDeviceCommandDelegate`, `VoiceAgent`, and
`ui_view`'s deliberately ESP-free `UiSnapshot`.

---

## 9. Open questions

- Does `conftest.py` already handle singleton reset between tests, or do the
  current suites just avoid the problem? Determines whether Phase 2 needs
  process isolation.
- Keep nanobind/pytest for new suites, or add a plain C++ target (doctest/
  Catch2) for the byte-level decoder tests? Python bindings are ergonomic for
  fixtures and assertions; a C++ target avoids writing binding glue for every
  class. **Leaning: stay with nanobind** for consistency with the four existing
  suites.
- Should the host build compile `media_player`/`gemini_live` sources into the
  existing `waveshare_host` module, or a second module? One module is simpler
  but slower to rebuild as it grows.
