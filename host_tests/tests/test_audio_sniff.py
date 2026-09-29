"""Format check for files in the alarm folder: only Opus (Ogg or WebM) plays."""

import shutil
import subprocess

import pytest
import waveshare_host as wh

HEAD = 4096   # what the upload route sniffs

needs_ffmpeg = pytest.mark.skipif(shutil.which("ffmpeg") is None, reason="ffmpeg not installed")


def encode(tmp_path, name, *codec):
    out = tmp_path / name
    subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=2",
                    *codec, str(out)], check=True)
    return out.read_bytes()


@needs_ffmpeg
@pytest.mark.parametrize("name, codec, expected", [
    ("a.ogg", ["-c:a", "libopus"], (True, "ogg-opus")),
    ("a.opus", ["-c:a", "libopus"], (True, "ogg-opus")),
    ("a.webm", ["-c:a", "libopus"], (True, "webm-opus")),
    ("v.ogg", ["-c:a", "libvorbis"], (False, "ogg-vorbis")),
    ("v.webm", ["-c:a", "libvorbis"], (False, "webm-vorbis")),
    ("f.ogg", ["-c:a", "flac"], (False, "ogg-flac")),
    ("a.mp3", ["-c:a", "libmp3lame"], (False, "mp3")),
    ("a.wav", [], (False, "wav")),
    ("a.flac", [], (False, "flac")),
    ("a.m4a", ["-c:a", "aac"], (False, "mp4")),
])
def test_encoded_files(tmp_path, name, codec, expected):
    assert wh.sniff_audio(encode(tmp_path, name, *codec)[:HEAD]) == expected


@needs_ffmpeg
def test_mp3_renamed_to_ogg(tmp_path):
    data = encode(tmp_path, "a.mp3", "-c:a", "libmp3lame")
    (tmp_path / "renamed.ogg").write_bytes(data)
    assert wh.sniff_audio((tmp_path / "renamed.ogg").read_bytes()[:HEAD]) == (False, "mp3")


@needs_ffmpeg
def test_mp3_without_id3_tag(tmp_path):
    data = encode(tmp_path, "a.mp3", "-c:a", "libmp3lame", "-id3v2_version", "0", "-write_xing", "0")
    assert wh.sniff_audio(data[:HEAD]) == (False, "mp3")


def test_too_short_and_unknown():
    assert wh.sniff_audio(b"OggS") == (False, "too short")
    assert wh.sniff_audio(b"") == (False, "too short")
    assert wh.sniff_audio(b"hello world, not audio" * 4) == (False, "unknown")


def test_ogg_page_truncated_before_packet():
    # A valid page header claiming 255 segments, cut before the packet.
    page = b"OggS" + bytes(22) + bytes([255]) + bytes(60)
    assert wh.sniff_audio(page) == (False, "ogg-other")
