# host_tests

Portable firmware code compiled natively and tested with pytest. The C++
sources are the firmware's own (no copies); `shims/` stands in for the few
ESP-IDF and FreeRTOS headers they include, and `bindings/` exposes them to
Python with nanobind as the `waveshare_host` module.

```bash
./host_tests/run_tests.sh                       # build + all tests
host_tests/.venv/bin/pytest host_tests/tests/test_nexus_db.py -q   # one file (after a build)
```

`run_tests.sh` creates `.venv` with pytest and nanobind on first use.

| Test | Covers |
|---|---|
| `test_sysdb_concurrency.py` | `EmbeddedSysDb` mutate/snapshot/change bits under threads |
| `test_buffer_manager.py` | Ring buffers |
| `test_resampler.py` | The linear resampler |
| `test_alert_mixer.py` | Alert mixing, fades, replacement |
| `test_nexus_db.py` | nexus_db format, recovery, merge, cleanup (schema `schema/testdb.star`) |
| `test_alarm_schedule.py`, `test_alarm_ring.py` | Alarm fire times, DST, snooze; the ringing state machine |
| `test_webm_seek.py` | WebM Cues, cluster search, block timecodes, Duration |
| `test_ogg_seek.py` | Ogg pages, header renumbering, the seek scan |
| `test_recording_probe.py` | Length and format of recordings (Ogg, WAV) |

Test data (the heads of two YouTube WebM files, two short Opus files) is in
`tests/data/`.

## Adding a test

Keep the logic you want to test free of ESP-IDF calls (the pattern used by
`AlarmRing`, `WebmSeek`, `OggSeek`), add its `.cpp` to `CMakeLists.txt`,
bind the functions in a `bindings/bind_*.cpp`, and write the pytest file.
