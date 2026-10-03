"""SessionGuard: the rules that end a stuck session."""

import waveshare_host as wh

V = wh.GuardVerdict
NONE = wh.SessionGuard.NONE
N = wh.SOUND_NO_WORDS_MS


def speech_run(g, start, end, step=30):
    t = start
    while t <= end:
        g.note_speech(t)
        t += step


def test_continuous_sound_without_words_ends():
    g = wh.SessionGuard()
    speech_run(g, 1000, 1000 + N - 100)
    assert g.check(1000 + N - 100, NONE, False, False) == V.Ok
    speech_run(g, 1000 + N - 70, 1000 + N + 100)
    assert g.check(1000 + N + 100, NONE, False, False) == V.SoundNoWords


def test_words_restart_the_count():
    g = wh.SessionGuard()
    speech_run(g, 0, N + 2000)
    # a transcription 1 s ago: the run counts from then
    assert g.check(N + 2000, N + 1000, False, False) == V.Ok
    speech_run(g, N + 2030, 2 * N + 1000)
    assert g.check(2 * N + 1000, N + 1000, False, False) == V.SoundNoWords


def test_gaps_split_the_run():
    g = wh.SessionGuard()
    speech_run(g, 0, 6000)
    speech_run(g, 6000 + wh.SPEECH_GAP_RESET_MS + 500, 6000 + wh.SPEECH_GAP_RESET_MS + 500 + 6000)
    # 12 s since the first sound, but the second run is only 6 s
    assert g.check(15500, NONE, False, False) == V.Ok


def test_reply_in_progress_never_counts():
    g = wh.SessionGuard()
    speech_run(g, 0, N + 5000)
    assert g.check(N + 5000, NONE, True, False) == V.Ok
    # the count starts when the reply ended
    speech_run(g, N + 5030, N + 5000 + N - 100)
    assert g.check(N + 5000 + N - 100, NONE, False, False) == V.Ok


def test_reply_hold_cap():
    g = wh.SessionGuard()
    assert not g.hold_expired(1000, True)
    assert not g.hold_expired(1000 + wh.REPLY_HOLD_MAX_MS - 1, True)
    assert g.hold_expired(1000 + wh.REPLY_HOLD_MAX_MS, True)


def test_reply_hold_resets_when_released():
    g = wh.SessionGuard()
    g.hold_expired(0, True)
    assert not g.hold_expired(10000, False)
    assert not g.hold_expired(wh.REPLY_HOLD_MAX_MS + 5000, True)   # a new hold began here


def test_check_reports_hold_expiry():
    g = wh.SessionGuard()
    g.check(0, NONE, True, True)
    assert g.check(wh.REPLY_HOLD_MAX_MS, NONE, True, True) == V.ReplyHoldExpired


def test_rate_limit():
    g = wh.SessionGuard()
    assert g.due_for_check(0)
    assert not g.due_for_check(100)
    assert g.due_for_check(300)


def test_reset_forgets():
    g = wh.SessionGuard()
    speech_run(g, 0, N + 100)
    g.reset()
    assert g.check(N + 100, NONE, False, False) == V.Ok
