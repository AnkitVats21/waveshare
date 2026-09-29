"""Alarm tone settings: built-in patterns, uploaded files, library songs."""

import pytest
import waveshare_host as wh


@pytest.mark.parametrize("tone, expected", [
    ("", ("builtin", "")),
    ("builtin", ("builtin", "")),
    ("builtin:chime", ("builtin", "chime")),
    ("file:wake up.ogg", ("file", "wake up.ogg")),
    ("dQw4w9WgXcQ", ("song", "dQw4w9WgXcQ")),
])
def test_parse_tone(tone, expected):
    assert wh.parse_tone(tone) == expected


@pytest.mark.parametrize("name", ["birds.ogg", "Wake Up-2.OPUS", "a_b.webm", "x" * 43 + ".opus"])
def test_valid_tone_file_names(name):
    assert wh.is_valid_tone_file_name(name)


@pytest.mark.parametrize("name", [
    "", ".hidden.ogg", " lead.ogg", "song.mp3", "noext", "../x.ogg", "a/b.ogg",
    "x" * 45 + ".opus", "semi;colon.ogg",
])
def test_invalid_tone_file_names(name):
    assert not wh.is_valid_tone_file_name(name)
