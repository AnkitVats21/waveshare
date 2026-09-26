"""AlertMixer: the alert track the speaker task pulls from PSRAM clips."""

import pytest
import waveshare_host as wh

FADE = wh.AlertMixer.FADE_SAMPLES


def const(value, n):
    return [value] * n


def drain(mixer, block=640, limit=1000):
    """Renders until idle; returns (all samples, list of events)."""
    out, events = [], []
    for _ in range(limit):
        pcm, ev = mixer.render(block)
        out += pcm
        if ev:
            events.append(ev)
        if not pcm and not mixer.active():
            # one more call reports "ended" if it is still owed
            pcm, ev = mixer.render(block)
            if ev:
                events.append(ev)
            break
    return out, events


@pytest.fixture
def mixer():
    return wh.AlertMixer()


def test_idle_renders_nothing(mixer):
    assert mixer.render(640) == ([], "")
    assert not mixer.active()
    assert mixer.current() == -1


def test_play_needs_a_clip_and_enabled_slot(mixer):
    assert not mixer.play(0)
    mixer.set_clip(0, const(1000, 100))
    mixer.set_enabled(0, False)
    assert not mixer.play(0)
    mixer.set_enabled(0, True)
    assert mixer.play(0)
    assert not mixer.play(wh.AlertMixer.SLOTS)


def test_clip_plays_sample_exact_with_events(mixer):
    clip = [(i * 37) % 20000 - 10000 for i in range(1500)]
    mixer.set_clip(1, clip)
    assert mixer.play(1)
    assert mixer.current() == 1
    out, events = drain(mixer)
    assert out == clip
    assert events == ["started", "ended"]
    assert not mixer.active()


def test_short_clip_in_one_render_still_reports_both_events(mixer):
    mixer.set_clip(0, const(500, 50))
    mixer.play(0)
    pcm, ev = mixer.render(640)
    assert pcm == const(500, 50) and ev == "started"
    assert mixer.render(640) == ([], "ended")
    assert mixer.render(640) == ([], "")


def test_gain_scales_and_saturates(mixer):
    mixer.set_clip(0, const(10000, 100))
    mixer.set_gain(0, 0.5)
    mixer.play(0)
    out, _ = drain(mixer)
    assert out == const(5000, 100)

    mixer.set_gain(0, 4.0)
    mixer.play(0)
    out, _ = drain(mixer)
    assert out == const(32767, 100)

    mixer.set_gain(0, 9.0)
    assert mixer.gain(0) == pytest.approx(4.0)


def test_new_alert_fades_out_the_playing_one_then_replaces_it(mixer):
    a, b = const(10000, 5000), const(-7000, 400)
    mixer.set_clip(0, a)
    mixer.set_clip(1, b)
    mixer.play(0)
    first, ev = mixer.render(100)
    assert first == const(10000, 100) and ev == "started"

    assert mixer.play(1)
    out, events = drain(mixer)
    fade, rest = out[:FADE], out[FADE:]
    assert all(0 <= s <= 10000 for s in fade)
    assert fade == sorted(fade, reverse=True)
    assert fade[-1] < 10000 // FADE * 2
    assert rest == b
    # one continuous alert as far as ducking is concerned
    assert events == ["ended"]


def test_burst_of_requests_plays_only_the_last(mixer):
    for slot, value in enumerate((100, 200, 300)):
        mixer.set_clip(slot, const(value, 50))
    mixer.play(0)
    mixer.play(1)
    mixer.play(2)
    assert mixer.current() == 2
    out, events = drain(mixer)
    assert out == const(300, 50)
    assert events == ["started", "ended"]


def test_replaying_the_same_alert_restarts_it(mixer):
    clip = list(range(1, 1001))
    mixer.set_clip(0, clip)
    mixer.play(0)
    mixer.render(500)
    mixer.play(0)
    out, _ = drain(mixer)
    assert out[FADE:] == clip
    assert out[0] <= 501


def test_stop_fades_out_and_ends(mixer):
    mixer.set_clip(0, const(8000, 10000))
    mixer.play(0)
    mixer.render(320)
    mixer.stop()
    out, events = drain(mixer)
    assert len(out) == FADE
    assert out == sorted(out, reverse=True)
    assert events == ["ended"]


def test_stop_near_the_end_fades_only_what_is_left(mixer):
    mixer.set_clip(0, const(8000, 400))
    mixer.play(0)
    mixer.render(350)
    mixer.stop()
    out, _ = drain(mixer)
    assert len(out) == 50


def test_stop_drops_a_pending_request(mixer):
    mixer.set_clip(0, const(1, 10))
    mixer.play(0)
    mixer.stop()
    assert not mixer.active()
    assert mixer.render(640) == ([], "")


def test_swapping_a_clip_keeps_the_playing_one(mixer):
    mixer.set_clip(0, const(111, 1000))
    mixer.play(0)
    head, _ = mixer.render(300)
    mixer.set_clip(0, const(222, 10))
    tail, _ = drain(mixer)
    assert head + tail == const(111, 1000)
    assert mixer.clip(0) == const(222, 10)
    mixer.play(0)
    out, _ = drain(mixer)
    assert out == const(222, 10)


def test_clearing_a_slot_stops_it_being_played(mixer):
    mixer.set_clip(0, const(1, 10))
    mixer.set_clip(0, [])
    assert not mixer.play(0)


def test_synthesized_tones_have_the_expected_shape():
    rate = 32000
    notes = [(440.0, 9000, 60, 10), (0.0, 0, 40, 0), (440.0, 9000, 60, 10)]
    pcm = wh.synthesize_tones(notes, rate)
    note, gap = rate * 60 // 1000, rate * 40 // 1000
    assert len(pcm) == 2 * note + gap
    assert pcm[note:note + gap] == [0] * gap
    assert max(abs(s) for s in pcm) <= 9000
    assert max(abs(s) for s in pcm) > 8500
    # the envelope starts and ends each note near zero
    assert abs(pcm[0]) == 0 and abs(pcm[note - 1]) < 100


def test_fade_edges_ramps_both_ends():
    pcm = wh.fade_edges(const(10000, 100), 10, 20)
    assert pcm[0] == 0
    assert pcm[9] == 9000
    assert pcm[10:80] == const(10000, 70)
    assert pcm[-1] == 0
    assert pcm[-20] == 9500
    assert wh.fade_edges(const(100, 5), 50, 50)[0] == 0
