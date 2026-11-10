"""KeyGesture: press, click and long press from a polled key level."""

import pytest
import waveshare_host as wh

POLL = 20
LONG = wh.LONG_PRESS_MS


def run(g, levels, start=0):
    """Feed (pressed, duration_ms) spans at the poll rate; return the events."""
    events, t = [], start
    for pressed, ms in levels:
        for _ in range(max(1, ms // POLL)):
            e = g.update(pressed, t)
            if e:
                events.append(e)
            t = (t + POLL) & 0xFFFFFFFF
    return events


@pytest.fixture
def g():
    return wh.KeyGesture()


def test_idle_gives_nothing(g):
    assert run(g, [(False, 1000)]) == []
    assert not g.down()


def test_tap_is_down_then_click(g):
    assert run(g, [(True, 100), (False, 40)]) == ["down", "click"]


def test_hold_is_down_then_long_once_and_no_click(g):
    assert run(g, [(True, 3000), (False, 40)]) == ["down", "long"]


def test_long_fires_at_the_threshold_while_held(g):
    assert g.update(True, 0) == "down"
    assert g.update(True, LONG - 1) == ""
    assert g.update(True, LONG) == "long"
    assert g.down()


def test_release_just_before_the_threshold_is_a_click(g):
    g.update(True, 0)
    g.update(True, LONG - POLL)
    assert g.update(False, LONG - 1) == "click"


def test_each_press_starts_fresh(g):
    assert run(g, [(True, 1000), (False, 100), (True, 100), (False, 40)]) == ["down", "long", "down", "click"]


def test_millisecond_counter_wrap(g):
    start = 0xFFFFFFFF - 100
    assert run(g, [(True, 1000), (False, 40)], start=start) == ["down", "long"]
