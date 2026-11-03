"""voice::decide(): what a session does after a reply.

A music command that succeeded (board or hub) closes the session once the
reply has played; a failed one (not found, ambiguous, error) leaves it open
for the answer; anything else waits for a follow-up with the music held."""
from waveshare_host import session_decide


def test_plain_reply_waits_for_a_follow_up():
    assert session_decide() == (False, 60000, "hold")


def test_other_tools_keep_the_session_open():
    assert session_decide(tools_ok=2, tools_failed=1) == (False, 60000, "hold")


def test_music_command_ends_the_session():
    assert session_decide(music_ok=1)[0] is True


def test_hub_music_command_ends_the_session():
    assert session_decide(hub_music_done=True)[0] is True


def test_failed_music_keeps_the_session_open_for_the_answer():
    assert session_decide(music_failed=1) == (False, 60000, "hold")
    # The old behaviour, as a rule.
    assert session_decide(music_failed=1, end_after_failed_music=True)[0] is True


def test_failed_then_successful_music_ends():
    assert session_decide(music_failed=1, music_ok=1)[0] is True


def test_follow_up_window_comes_from_the_rules():
    assert session_decide(followup_ms=8000) == (False, 8000, "hold")
