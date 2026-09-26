"""AlarmRing: ringing, snooze, ring limit and the built-in fallback."""

import pytest
import waveshare_host as wh

MIN = 60_000


@pytest.fixture
def ring():
    return wh.AlarmRing(ring_limit_ms=10 * MIN, snooze_ms=9 * MIN, watchdog_ms=3000)


def test_idle_ignores_everything(ring):
    for call in (ring.stop, ring.snooze, ring.song_progress, ring.song_ended, ring.song_failed, ring.tick):
        assert call(0) == ""
    assert ring.state() == "idle"


def test_fire_with_song_plays_it_and_stop_finishes(ring):
    assert ring.fire(3, True, 1000) == "song"
    assert ring.state() == "ringing" and ring.source() == "song" and ring.alarm_id() == 3
    assert ring.stop(2000) == "finish"
    assert ring.state() == "idle" and ring.last_end() == "stopped"


def test_fire_without_song_plays_builtin(ring):
    assert ring.fire(1, False, 0) == "builtin"
    assert ring.source() == "builtin"


def test_second_alarm_while_ringing_is_ignored(ring):
    ring.fire(1, True, 0)
    assert ring.fire(2, True, 10) == ""
    assert ring.alarm_id() == 1


def test_song_restarts_when_it_ends(ring):
    ring.fire(1, True, 0)
    ring.song_progress(500)
    assert ring.song_ended(90_000) == "song"
    assert ring.source() == "song"


def test_song_that_ends_without_playing_falls_back(ring):
    ring.fire(1, True, 0)
    assert ring.song_ended(200) == "builtin"


def test_song_error_falls_back_to_builtin(ring):
    ring.fire(1, True, 0)
    ring.song_progress(100)
    assert ring.song_failed(5000) == "builtin"
    assert ring.source() == "builtin"
    # the tone keeps going; later song reports are stale
    assert ring.song_ended(6000) == ""


def test_watchdog_catches_a_silent_song(ring):
    ring.fire(1, True, 0)
    assert ring.tick(2999) == ""
    assert ring.tick(3000) == "builtin"


def test_watchdog_is_satisfied_by_progress(ring):
    ring.fire(1, True, 0)
    ring.song_progress(800)
    assert ring.tick(5000) == ""
    assert ring.source() == "song"


def test_watchdog_rearms_on_restart(ring):
    ring.fire(1, True, 0)
    ring.song_progress(100)
    ring.song_ended(60_000)
    assert ring.tick(63_000) == "builtin"


def test_ring_limit_times_out(ring):
    ring.fire(1, False, 0)
    assert ring.tick(10 * MIN - 1) == ""
    assert ring.tick(10 * MIN) == "finish"
    assert ring.state() == "idle" and ring.last_end() == "timed_out"


def test_snooze_silences_then_rings_again(ring):
    ring.fire(1, True, 0)
    ring.song_progress(100)
    assert ring.snooze(60_000) == "silence"
    assert ring.state() == "snoozed" and ring.snooze_count() == 1
    assert ring.snooze_left_ms(60_000 + MIN) == 8 * MIN
    assert ring.tick(60_000 + 9 * MIN - 1) == ""
    assert ring.tick(60_000 + 9 * MIN) == "song"
    assert ring.state() == "ringing"
    # the ring limit counts from the end of the snooze
    assert ring.ringing_ms(60_000 + 10 * MIN) == MIN


def test_snooze_only_while_ringing(ring):
    ring.fire(1, True, 0)
    ring.snooze(10)
    assert ring.snooze(20) == ""


def test_stop_during_snooze_finishes(ring):
    ring.fire(1, True, 0)
    ring.snooze(10)
    assert ring.stop(20) == "finish"
    assert ring.tick(10 * MIN) == ""


def test_fallback_holds_across_snoozes(ring):
    ring.fire(1, True, 0)
    ring.song_failed(100)
    ring.snooze(1000)
    assert ring.tick(1000 + 9 * MIN) == "builtin"


def test_new_alarm_replaces_a_snoozed_one(ring):
    ring.fire(1, False, 0)
    ring.snooze(10)
    assert ring.fire(2, True, 20) == "song"
    assert ring.alarm_id() == 2 and ring.snooze_count() == 0


def test_timeout_after_snooze_rerings(ring):
    ring.fire(1, True, 0)
    ring.song_progress(1)
    ring.snooze(MIN)
    ring.tick(10 * MIN)                     # rings again
    ring.song_progress(10 * MIN + 1)
    assert ring.tick(20 * MIN - 1) == ""
    assert ring.tick(20 * MIN) == "finish"
