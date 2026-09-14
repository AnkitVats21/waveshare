import threading
import time
import pytest
import waveshare_host as wh


def test_sysdb_defaults(sysdb):
    """Verify initial default values in SysDb."""
    assert sysdb.speaker_volume() == 80
    assert sysdb.mic_gain() == pytest.approx(60.0)
    assert sysdb.mic_enabled() is True
    assert sysdb.wifi_connected() is False
    assert sysdb.assistant_speaking() is False
    assert sysdb.turn_complete_pending() is False
    assert sysdb.session_state() == wh.AssistantState.Idle
    assert sysdb.pipeline_mode() == wh.PipelineMode.WAKE_IDLE
    assert sysdb.media_state() == wh.MediaPlaybackState.IDLE
    assert sysdb.is_media_ducked() is False


def test_sysdb_mutation_and_snapshot_isolation(sysdb):
    """Verify that snapshots are immutable copies and do not mutate retroactively."""
    snap1 = sysdb.snapshot()
    assert snap1.speaker_volume == 80

    def mutate_volume(s):
        s.speaker_volume = 45

    sysdb.mutate(mutate_volume)

    # Directly queried getter reflects change
    assert sysdb.speaker_volume() == 45
    # Previous snapshot remains unchanged (snapshot isolation)
    assert snap1.speaker_volume == 80

    # New snapshot reflects change
    snap2 = sysdb.snapshot()
    assert snap2.speaker_volume == 45


def test_sysdb_hot_path_audio_flags(sysdb):
    """Verify atomic hot-path audio flags update instantly in mutate()."""
    # Initially all false
    assert sysdb.hot_assistant_speaking() is False
    assert sysdb.hot_turn_complete_pending() is False
    assert sysdb.hot_companion_connected() is False
    assert sysdb.hot_companion_settled() is False

    # Turn on assistant speaking
    sysdb.mutate(lambda s: setattr(s, "assistant_speaking", True))
    assert sysdb.hot_assistant_speaking() is True
    assert sysdb.assistant_speaking() is True
    assert (sysdb.hot_audio_flags() & 1) != 0

    # Turn on companion flags
    def set_companion(s):
        s.companion_connected = True
        s.companion_settled = True

    sysdb.mutate(set_companion)
    assert sysdb.hot_companion_connected() is True
    assert sysdb.hot_companion_settled() is True

    # Clear assistant speaking
    sysdb.mutate(lambda s: setattr(s, "assistant_speaking", False))
    assert sysdb.hot_assistant_speaking() is False
    # Companion should still be true
    assert sysdb.hot_companion_connected() is True


def test_sysdb_string_fields_safety(sysdb):
    """Verify string properties handle arbitrary lengths and UTF-8 gracefully."""
    song_id = "yt_ABC123XYZ"
    title = "Bohemian Rhapsody"

    def set_media(s):
        s.active_song_id = song_id
        s.media_title = title

    sysdb.mutate(set_media)

    snap = sysdb.snapshot()
    assert snap.active_song_id == song_id
    assert snap.media_title == title

    # Test long string truncation safety (buffer is 64 bytes)
    very_long = "A" * 128
    sysdb.mutate(lambda s: setattr(s, "media_title", very_long))
    snap = sysdb.snapshot()
    assert len(snap.media_title) <= 63


def test_sysdb_multithreaded_concurrency_stress(sysdb):
    """Stress test EmbeddedSysDb reader-writer lock with concurrent readers and writers."""
    stop_event = threading.Event()
    errors = []
    iterations = 2000

    def writer_task():
        try:
            for i in range(iterations):
                vol = (i % 100)
                speaking = (i % 2 == 0)
                ducked = (i % 3 == 0)

                def update(s):
                    s.speaker_volume = vol
                    s.assistant_speaking = speaking
                    s.is_ducked = ducked

                sysdb.mutate(update)
        except Exception as e:
            errors.append(f"Writer error: {e}")

    def reader_task(reader_id):
        try:
            while not stop_event.is_set():
                # Perform both full snapshot and direct field reads
                snap = sysdb.snapshot()
                vol = sysdb.speaker_volume()
                speaking = sysdb.hot_assistant_speaking()
                ducked = sysdb.is_media_ducked()

                # Basic invariant checks: volume must be 0..99
                assert 0 <= snap.speaker_volume <= 99
                assert 0 <= vol <= 99
                assert isinstance(speaking, bool)
                assert isinstance(ducked, bool)
        except Exception as e:
            errors.append(f"Reader {reader_id} error: {e}")

    writer = threading.Thread(target=writer_task)
    readers = [threading.Thread(target=reader_task, args=(i,)) for i in range(6)]

    for r in readers:
        r.start()
    writer.start()

    writer.join(timeout=10.0)
    assert not writer.is_alive(), "Writer thread hung or deadlocked!"

    stop_event.set()
    for r in readers:
        r.join(timeout=2.0)
        assert not r.is_alive(), "Reader thread hung or deadlocked!"

    assert len(errors) == 0, f"Concurrency stress errors: {errors}"
