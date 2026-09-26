"""nextFire: repeat days, one-shots, midnight and DST, in the process timezone."""

import datetime as dt
from zoneinfo import ZoneInfo

import pytest
import waveshare_host as wh

MON, TUE, WED, THU, FRI, SAT, SUN = (1 << i for i in range(7))
EVERY_DAY = 0x7F


def epoch(tz, *args):
    return int(dt.datetime(*args, tzinfo=ZoneInfo(tz)).timestamp())


def local(tz, t):
    return dt.datetime.fromtimestamp(t, ZoneInfo(tz))


@pytest.fixture
def ist():
    wh.set_tz("IST-5:30")
    yield "Asia/Kolkata"
    wh.set_tz("UTC")


@pytest.fixture
def cet():
    wh.set_tz("CET-1CEST,M3.5.0,M10.5.0/3")
    yield "Europe/Berlin"
    wh.set_tz("UTC")


def test_daily_later_today(ist):
    now = epoch(ist, 2026, 9, 27, 5, 0)
    assert wh.next_fire(5, 30, EVERY_DAY, after=now) == epoch(ist, 2026, 9, 27, 5, 30)


def test_daily_already_past_goes_to_tomorrow(ist):
    now = epoch(ist, 2026, 9, 27, 6, 0)
    assert wh.next_fire(5, 30, EVERY_DAY, after=now) == epoch(ist, 2026, 9, 28, 5, 30)


def test_strictly_after(ist):
    # At the fire time itself the next one is a day later, so it fires once.
    t = epoch(ist, 2026, 9, 27, 5, 30)
    assert wh.next_fire(5, 30, EVERY_DAY, after=t) == t + 86400
    assert wh.next_fire(5, 30, EVERY_DAY, after=t - 1) == t


def test_midnight(ist):
    now = epoch(ist, 2026, 9, 27, 23, 59, 30)
    assert wh.next_fire(0, 0, EVERY_DAY, after=now) == epoch(ist, 2026, 9, 28, 0, 0)


def test_repeat_days_skip_to_the_next_set_day(ist):
    # 2026-09-27 is a Sunday.
    now = epoch(ist, 2026, 9, 27, 12, 0)
    assert local(ist, now).weekday() == 6
    assert wh.next_fire(7, 0, MON | WED | FRI, after=now) == epoch(ist, 2026, 9, 28, 7, 0)
    assert wh.next_fire(7, 0, WED, after=now) == epoch(ist, 2026, 9, 30, 7, 0)


def test_weekly_on_the_same_day_but_past_waits_a_week(ist):
    now = epoch(ist, 2026, 9, 27, 12, 0)   # Sunday noon
    assert wh.next_fire(7, 0, SUN, after=now) == epoch(ist, 2026, 10, 4, 7, 0)


def test_weekends(ist):
    fri = epoch(ist, 2026, 10, 2, 9, 0)
    fires = [local(ist, t).strftime("%a") for t in _series(fri, 8, 0, SAT | SUN, 4)]
    assert fires == ["Sat", "Sun", "Sat", "Sun"]


def test_no_days_is_the_next_occurrence(ist):
    now = epoch(ist, 2026, 9, 27, 12, 0)
    assert wh.next_fire(11, 0, 0, after=now) == epoch(ist, 2026, 9, 28, 11, 0)
    assert wh.next_fire(13, 0, 0, after=now) == epoch(ist, 2026, 9, 27, 13, 0)


def test_fixed_time(ist):
    at = epoch(ist, 2026, 9, 27, 13, 20, 15)   # timers keep the seconds
    assert wh.next_fire(0, 0, 0, at=at, after=at - 1) == at
    assert wh.next_fire(0, 0, 0, at=at, after=at) == 0
    assert wh.next_fire(0, 0, 0, at=at, after=at + 3600) == 0


def test_invalid_time_never_fires(ist):
    assert wh.next_fire(24, 0, EVERY_DAY, after=0) == 0
    assert wh.next_fire(7, 60, EVERY_DAY, after=0) == 0


def test_utc():
    wh.set_tz("UTC")
    now = epoch("UTC", 2026, 9, 27, 0, 0)
    assert wh.next_fire(6, 15, EVERY_DAY, after=now) == epoch("UTC", 2026, 9, 27, 6, 15)


def _series(after, hour, minute, days, n):
    out = []
    for _ in range(n):
        after = wh.next_fire(hour, minute, days, after=after)
        assert after
        out.append(after)
    return out


def test_dst_keeps_local_time_across_the_changes(cet):
    # CEST starts 2026-03-29, ends 2026-10-25.
    for start in (epoch(cet, 2026, 3, 27, 12, 0), epoch(cet, 2026, 10, 23, 12, 0)):
        fires = _series(start, 7, 0, EVERY_DAY, 5)
        assert [local(cet, t).strftime("%H:%M") for t in fires] == ["07:00"] * 5
        assert len({local(cet, t).date() for t in fires}) == 5


def test_dst_gap_fires_once_that_day(cet):
    # 02:30 does not exist on 2026-03-29; it still fires that morning, once.
    fires = _series(epoch(cet, 2026, 3, 28, 12, 0), 2, 30, EVERY_DAY, 3)
    days = [local(cet, t).date() for t in fires]
    assert days == [dt.date(2026, 3, 29), dt.date(2026, 3, 30), dt.date(2026, 3, 31)]
    assert 1 <= local(cet, fires[0]).hour <= 3
    assert local(cet, fires[1]).strftime("%H:%M") == "02:30"


def test_dst_repeated_hour_fires_once(cet):
    # 02:30 happens twice on 2026-10-25; it fires on one of them only.
    fires = _series(epoch(cet, 2026, 10, 24, 12, 0), 2, 30, EVERY_DAY, 3)
    days = [local(cet, t).date() for t in fires]
    assert days == [dt.date(2026, 10, 25), dt.date(2026, 10, 26), dt.date(2026, 10, 27)]


# ── Words from the voice tools ───────────────────────────────────────────────

@pytest.mark.parametrize("text,days", [
    ("", 0), ("once", 0),
    ("daily", EVERY_DAY), ("every day", EVERY_DAY), ("Everyday", EVERY_DAY),
    ("weekdays", 0x1F), ("weekends", SAT | SUN),
    ("mon", MON), ("Mondays", MON), ("tues", TUE), ("thursday", THU),
    ("mon, wed and fri", MON | WED | FRI), ("sat/sun", SAT | SUN), ("every Monday", MON),
])
def test_parse_days(text, days):
    assert wh.parse_days(text) == days


@pytest.mark.parametrize("text", ["fortnightly", "mo", "monday, someday", "xyz"])
def test_parse_days_rejects(text):
    assert wh.parse_days(text) is None


@pytest.mark.parametrize("days,text", [
    (0, "once"), (EVERY_DAY, "every day"), (0x1F, "weekdays"), (SAT | SUN, "weekends"),
    (MON | WED | FRI, "Mon, Wed, Fri"), (SUN, "Sun"),
])
def test_format_days(days, text):
    assert wh.format_days(days) == text


def test_resolve_day(ist):
    now = epoch(ist, 2026, 9, 27, 12, 0)   # Sunday noon
    assert wh.resolve_day("today", 17, 0, now) == epoch(ist, 2026, 9, 27, 17, 0)
    assert wh.resolve_day("Tomorrow", 7, 30, now) == epoch(ist, 2026, 9, 28, 7, 30)
    assert wh.resolve_day("2026-12-31", 23, 59, now) == epoch(ist, 2026, 12, 31, 23, 59)
    assert wh.resolve_day("wednesday", 9, 0, now) == epoch(ist, 2026, 9, 30, 9, 0)
    # The same day name: today if still ahead, else a week on.
    assert wh.resolve_day("sunday", 18, 0, now) == epoch(ist, 2026, 9, 27, 18, 0)
    assert wh.resolve_day("sunday", 8, 0, now) == epoch(ist, 2026, 10, 4, 8, 0)


def test_resolve_day_month_end(ist):
    now = epoch(ist, 2026, 12, 31, 22, 0)
    assert wh.resolve_day("tomorrow", 6, 0, now) == epoch(ist, 2027, 1, 1, 6, 0)


def test_resolve_day_errors(ist):
    now = epoch(ist, 2026, 9, 27, 12, 0)
    assert wh.resolve_day("today", 9, 0, now) == wh.DAY_PAST
    assert wh.resolve_day("2026-01-01", 9, 0, now) == wh.DAY_PAST
    assert wh.resolve_day("someday", 9, 0, now) == wh.DAY_INVALID
    assert wh.resolve_day("2026-02-30", 9, 0, now) == wh.DAY_INVALID
    assert wh.resolve_day("today", 24, 0, now) == wh.DAY_INVALID
