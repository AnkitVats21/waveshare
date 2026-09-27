"""Recording length and format from the file's bytes (RecordingProbe), as
recordings.ndb reads them for the recorder and the scan of /sdcard/recordings.

The .opus fixtures were made by ffmpeg/libopus (a sine, 2.5 s stereo at 24 kHz and
1.5 s mono at 16 kHz), so they also check the Ogg CRC.
"""

import struct
from pathlib import Path

import pytest
import waveshare_host as wh

DATA = Path(__file__).parent / "data"
HEAD, TAIL = wh.PROBE_HEAD_BYTES, wh.PROBE_TAIL_BYTES


def probe_file(data):
    return wh.probe_opus(data[:HEAD], data[-TAIL:])


@pytest.mark.parametrize("name,expected", [
    ("stereo_2500ms.opus", (2500, 24000, 2)),  # libopus has no 32 kHz, so ffmpeg fed it 24 kHz
    ("mono_1500ms.opus", (1500, 16000, 1)),
])
def test_opus_length_and_format(name, expected):
    assert probe_file((DATA / name).read_bytes()) == expected


def test_opus_cut_off_final_page_uses_the_page_before():
    data = (DATA / "stereo_2500ms.opus").read_bytes()
    last = data.rfind(b"OggS")
    duration, _, _ = probe_file(data[:last + 40])  # the last page cut short
    assert 0 < duration < 2500


def test_opus_ignores_a_fake_page_in_trailing_garbage():
    data = (DATA / "mono_1500ms.opus").read_bytes()
    fake = b"OggS\x00\x04" + struct.pack("<Q", 48000 * 99) + bytes(13)
    assert probe_file(data + fake + b"junk")[0] == 1500


def test_opus_tail_needs_only_the_last_page():
    data = (DATA / "stereo_2500ms.opus").read_bytes()
    last = data.rfind(b"OggS")
    assert wh.probe_opus(data[:HEAD], data[last:])[0] == 2500
    assert wh.probe_opus(data[:HEAD], data[last + 1:]) is None


def test_opus_rejects_other_files():
    assert wh.probe_opus(b"RIFF" + bytes(600), b"") is None
    webm = (DATA / "2SUwOgmvzK4.head.webm").read_bytes()
    assert wh.probe_opus(webm[:HEAD], webm[-TAIL:]) is None
    ogg = (DATA / "mono_1500ms.opus").read_bytes()
    assert wh.probe_opus(ogg[:HEAD], b"") is None


def wav(rate=16000, channels=1, bits=16, frames=16000, data_size=None, extra=b""):
    block = channels * bits // 8
    pcm = bytes(frames * block)
    size = len(pcm) if data_size is None else data_size
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * block, block, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + extra + b"data" + struct.pack("<I", size) + pcm
    return b"RIFF" + struct.pack("<I", len(body)) + body


def test_wav_length_and_format():
    w = wav(rate=32000, channels=2, frames=32000 * 3)
    assert wh.probe_wav(w[:HEAD], len(w)) == (3000, 32000, 2)


def test_wav_skips_other_chunks():
    w = wav(frames=8000, extra=b"LIST" + struct.pack("<I", 5) + b"abcde\x00")
    assert wh.probe_wav(w[:HEAD], len(w)) == (500, 16000, 1)


@pytest.mark.parametrize("data_size", [0, 0xFFFFFFFF])
def test_unfinished_wav_uses_the_bytes_present(data_size):
    w = wav(frames=24000, data_size=data_size)
    assert wh.probe_wav(w[:HEAD], len(w)) == (1500, 16000, 1)


def test_wav_rejects_other_files():
    ogg = (DATA / "mono_1500ms.opus").read_bytes()
    assert wh.probe_wav(ogg[:HEAD], len(ogg)) is None
    assert wh.probe_wav(b"RIFF\x00\x00\x00\x00WAVEdata\xff\xff\xff\xff", 100) is None  # no fmt
    assert wh.probe_wav(b"RIFF\x00\x00\x00\x00WAVEjunk\xff\xff\xff\x7f", 100) is None
