import math

import pytest

import waveshare_host as wh

RATE = 32000


def gain_db(db, freq):
    """Steady-state gain of the filter at `freq`, from a 0.5 s sine."""
    f = wh.TrebleFilter()
    f.set(db, RATE)
    n = RATE // 2
    x = [0.5 * math.sin(2 * math.pi * freq * i / RATE) for i in range(n)]
    y = f.process(x)
    tail = n // 2  # skip the settling
    rms = lambda s: math.sqrt(sum(v * v for v in s[tail:]) / (n - tail))
    return 20 * math.log10(rms(y) / rms(x))


def test_zero_is_bypass():
    f = wh.TrebleFilter()
    f.set(0, RATE)
    x = [0.1, -0.7, 0.3, 0.9]
    assert f.process(x) == [pytest.approx(v) for v in x]


def test_lows_untouched_highs_cut():
    for db in (-3, -6, -12):
        assert abs(gain_db(db, 100)) < 0.2
        assert abs(gain_db(db, 12000) - db) < 0.5
        # Half the cut at the corner (shelf midpoint).
        assert abs(gain_db(db, wh.TREBLE_CORNER_HZ) - db / 2) < 0.5


def test_clamped():
    f = wh.TrebleFilter()
    f.set(-40, RATE)
    assert f.db() == wh.TREBLE_MIN_DB
    f.set(5, RATE)
    assert f.db() == 0


def test_never_louder():
    # A cut never raises the level anywhere (no overshoot past 0 dB).
    for freq in (50, 500, 1500, 3000, 6000, 15000):
        assert gain_db(-6, freq) < 0.1
