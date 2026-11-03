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


# ── Errors and the fallback model ──
from waveshare_host import session_classify_error, session_pick_model


def test_error_kinds_from_gemini_close_reasons():
    assert session_classify_error(1011, "You exceeded your current quota, please check your plan") == "quota"
    assert session_classify_error(1011, "RESOURCE_EXHAUSTED") == "quota"
    assert session_classify_error(1008, "Method doesn't allow unregistered callers (callers without established identity). Please use API Key") == "auth"
    assert session_classify_error(0, "the hub has no Gemini key") == "auth"
    assert session_classify_error(1008, "models/gemini-x-live is not found for API version v1beta") == "model"
    assert session_classify_error(-1, "the hub link dropped") == "network"
    assert session_classify_error(1006, "") == "network"
    assert session_classify_error(1011, "Internal error encountered.") == "other"


def test_model_fallback_only_while_active_and_never_to_itself():
    fb = "gemini-3.1-flash-live-preview"
    assert session_pick_model("gemini-3.8-live", False, fb) == "gemini-3.8-live"
    assert session_pick_model("gemini-3.8-live", True, fb) == fb
    assert session_pick_model("models/gemini-3.1-flash-live-preview", True, fb) == "models/gemini-3.1-flash-live-preview"
    assert session_pick_model("gemini-3.8-live", True, "") == "gemini-3.8-live"
