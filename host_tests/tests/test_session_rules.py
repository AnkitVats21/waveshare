"""voice::decide(): what a session does after a reply.

The defaults reproduce the behaviour before voice_session: a music command
(board or hub) closes the session once the reply has played, even a failed
one; anything else waits 60 s for a follow-up with the music held."""
from waveshare_host import session_decide


def test_plain_reply_waits_for_a_follow_up():
    assert session_decide() == (False, 60000, "hold")


def test_other_tools_keep_the_session_open():
    assert session_decide(tools_ok=2, tools_failed=1) == (False, 60000, "hold")


def test_music_command_ends_the_session():
    assert session_decide(music_ok=1)[0] is True


def test_hub_music_command_ends_the_session():
    assert session_decide(hub_music_done=True)[0] is True


def test_failed_music_ends_by_default_and_not_when_turned_off():
    assert session_decide(music_failed=1)[0] is True
    end, window, music = session_decide(music_failed=1, end_after_failed_music=False)
    assert (end, window, music) == (False, 60000, "hold")


def test_follow_up_window_comes_from_the_rules():
    assert session_decide(followup_ms=8000) == (False, 8000, "hold")
