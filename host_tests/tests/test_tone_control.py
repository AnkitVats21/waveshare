import math

import pytest

import waveshare_host as wh

RATE = 32000


def gain_db(bass, treble, freq):
    """Steady-state gain of the tone control at `freq`, from a 0.5 s sine."""
    t = wh.ToneControl()
    t.set(bass, treble, RATE)
    n = RATE // 2
    x = [0.5 * math.sin(2 * math.pi * freq * i / RATE) for i in range(n)]
    y = t.process(x)
    tail = n // 2  # skip the settling
    rms = lambda s: math.sqrt(sum(v * v for v in s[tail:]) / (n - tail))
    return 20 * math.log10(rms(y) / rms(x))


def test_flat_is_bypass():
    t = wh.ToneControl()
    t.set(0, 0, RATE)
    x = [0.1, -0.7, 0.3, 0.9]
    assert t.process(x) == [pytest.approx(v) for v in x]


@pytest.mark.parametrize("db", [-3, -6, -12])
def test_treble_cut(db):
    assert abs(gain_db(0, db, 100)) < 0.2
    assert abs(gain_db(0, db, 12000) - db) < 0.5
    # Half the cut at the corner (shelf midpoint).
    assert abs(gain_db(0, db, wh.TONE_TREBLE_HZ) - db / 2) < 0.5


@pytest.mark.parametrize("db", [-6, -12])
def test_bass_cut(db):
    assert abs(gain_db(db, 0, 30) - db) < 0.5
    assert abs(gain_db(db, 0, wh.TONE_BASS_HZ) - db / 2) < 0.5
    assert abs(gain_db(db, 0, 8000)) < 0.2


def test_boost_keeps_headroom():
    # +6 bass: the lows stay at unity, everything else drops 6 dB, so no
    # frequency comes out louder than it went in.
    assert abs(gain_db(6, 0, 30)) < 0.5
    assert abs(gain_db(6, 0, 8000) + 6) < 0.3
    for freq in (30, 100, 250, 1000, 3000, 12000):
        assert gain_db(6, 6, freq) < 0.1
        assert gain_db(-12, -12, freq) < 0.1


def test_clamped():
    t = wh.ToneControl()
    t.set(-40, 40, RATE)
    assert t.bass_db() == wh.TONE_MIN_DB
    assert t.treble_db() == wh.TONE_MAX_DB
