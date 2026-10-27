"""Spoken profile names for switch_profile (main/services/profiles/ProfileMatch)."""
import waveshare_host as wh

NAMES = ["Default", "Yoga teacher", "Technical companion", "Tech news"]


def match(query, names=NAMES):
    return wh.profile_match(names, query)


def test_exact():
    assert match("Yoga teacher") == 1
    assert match("Default") == 0


def test_case_and_spaces():
    assert match("yoga TEACHER") == 1
    assert match("  yoga   teacher ") == 1


def test_partial():
    assert match("yoga") == 1
    assert match("companion") == 2
    assert match("the yoga one") == 1
    assert match("technical profile") == 2
    assert match("my default persona") == 0


def test_ambiguous():
    # "tech" is in "Technical companion" and "Tech news"
    assert match("tech") == wh.PROFILE_AMBIGUOUS
    # an exact name still wins over the names containing it
    assert match("tech", ["Tech", "Tech news"]) == 0


def test_none():
    assert match("chef") == wh.PROFILE_NONE
    assert match("") == wh.PROFILE_NONE
    assert match("   ") == wh.PROFILE_NONE
    assert match("yoga", []) == wh.PROFILE_NONE


def test_fillers_alone_are_a_name():
    assert match("the one", ["The one"]) == 0
    assert match("profile", ["Work profile"]) == 0


def test_normalize_and_same_name():
    assert wh.profile_normalize("  Yoga \t Teacher ") == "yoga teacher"
    assert wh.profile_same_name("yoga teacher", "Yoga  Teacher")
    assert not wh.profile_same_name("yoga", "yoga teacher")
