"""Ogg Opus seeking (OggSeek): the header pages kept for seeks, renumbered to
run into the first kept page, and the scan for that page from an estimated
position.

Fixture stereo_2500ms.opus (ffmpeg): pages 0 OpusHead, 1 OpusTags, then
audio pages ending at granules 48000, 96000 and 120312 (EOS); pre-skip 312.
"""

import struct
from pathlib import Path

import pytest
import waveshare_host as wh

DATA = Path(__file__).parent / "data"
STEREO = (DATA / "stereo_2500ms.opus").read_bytes()
PAGE_AT = [0, 47, 137, 2218, 4401]   # byte offset of each page


def serial_and_preskip(data=STEREO):
    serial, pre_skip, _, _ = wh.parse_ogg_header(data)
    return serial, pre_skip


def scan(data, target_ms, chunk=512):
    serial, pre_skip = serial_and_preskip()
    return wh.ogg_seek_scan(data, serial, pre_skip, target_ms, chunk)


# ── Pages ────────────────────────────────────────────────────────────────────

def test_reads_every_page():
    for i, at in enumerate(PAGE_AT):
        status, _, _, seq, _, length = wh.read_ogg_page(STEREO[at:])
        assert status == "ok" and seq == i
        assert at + length == (PAGE_AT + [len(STEREO)])[i + 1]


def test_page_need_more_and_not_page():
    assert wh.read_ogg_page(STEREO[137:137 + 20])[0] == "need_more"      # header cut
    assert wh.read_ogg_page(STEREO[137:137 + 500])[0] == "need_more"     # body cut
    assert wh.read_ogg_page(STEREO[138:])[0] == "not_page"
    assert wh.read_ogg_page(b"Og")[0] == "need_more"
    bad = bytearray(STEREO[137:2218])
    bad[100] ^= 0xFF                                                       # CRC fails
    assert wh.read_ogg_page(bytes(bad))[0] == "not_page"


# ── Header ───────────────────────────────────────────────────────────────────

def test_header_is_the_pages_before_audio():
    serial, pre_skip, pages, header = wh.parse_ogg_header(STEREO)
    assert (pre_skip, pages) == (312, 2)
    assert header == STEREO[:137]
    assert wh.read_ogg_page(STEREO)[2] == serial


def test_header_needs_the_first_audio_page():
    assert wh.parse_ogg_header(STEREO[:137]) is None      # can't tell the tags have ended
    assert wh.parse_ogg_header(STEREO[:137 + 27 + 30]) is None
    assert wh.parse_ogg_header(STEREO[:2218]) is not None
    assert wh.parse_ogg_header(b"RIFF" + bytes(200)) is None


def test_renumbered_header_runs_into_the_page():
    out = wh.renumber_ogg_header(STEREO, 7)
    assert len(out) == 137
    first = wh.read_ogg_page(out)
    second = wh.read_ogg_page(out[first[5]:])
    assert first[0] == second[0] == "ok"                   # CRCs rewritten
    assert (first[3], second[3]) == (5, 6)
    assert wh.renumber_ogg_header(STEREO, 2) == STEREO[:137]
    assert wh.renumber_ogg_header(STEREO, 1) == b""


# ── Scan ─────────────────────────────────────────────────────────────────────

def test_seek_mid_page_starts_at_the_page_holding_the_target():
    # 1.5 s: page 2 ends at 0.99 s, page 3 holds the target. The decoder's
    # output from page 3 starts at granule 48000 (after its pre-skip), so
    # 0.5 s is thrown away.
    found, dropped, seq, discard = scan(STEREO[PAGE_AT[2]:], 1500)
    assert found and seq == 3 and discard == 72000 - 48000
    assert PAGE_AT[2] + dropped == PAGE_AT[3]


def test_seek_to_the_last_page():
    # Page 4 (EOS) starts where page 3 ended, 96000; 2.2 s is 105600.
    found, dropped, seq, discard = scan(STEREO[PAGE_AT[3]:], 2200)
    assert (found, seq, discard) == (True, 4, 105600 - 96000)
    # Landing inside page 3, the EOS page's start is unknown: it plays from
    # its start, at most one page early.
    found, dropped, seq, discard = scan(STEREO[2300:], 2200)
    assert (found, seq, discard) == (True, 4, 0)
    assert 2300 + dropped == PAGE_AT[4]


def test_page_start_counted_from_packets():
    # Pages 2 and 3 start where the one before ended; the EOS page's granule
    # trims its end, so it has no counted start.
    starts = [wh.ogg_page_start_granule(STEREO[at:]) for at in PAGE_AT[2:]]
    assert starts == [0, 48000, -1]


def test_mid_page_landing_uses_the_counted_start():
    # Landed inside page 2, so its end wasn't seen: page 3's start is counted
    # from its packets. Same result as reading page 2.
    found, dropped, seq, discard = scan(STEREO[1000:], 1500)
    assert (found, seq, discard) == (True, 3, 72000 - 48000)
    assert 1000 + dropped == PAGE_AT[3]


def test_seek_from_the_start():
    found, dropped, seq, discard = scan(STEREO, 0)
    assert (found, dropped, seq, discard) == (True, PAGE_AT[2], 2, 0)
    found, dropped, seq, discard = scan(STEREO, 400)
    assert (seq, discard) == (2, 400 * 48)                  # after the tags page (granule 0)


@pytest.mark.parametrize("chunk", [1, 7, 100, 4096])
def test_chunking_does_not_change_the_result(chunk):
    assert scan(STEREO[1000:], 1500, chunk) == scan(STEREO[1000:], 1500, 10**6)


def test_seek_past_the_end_is_not_found():
    assert scan(STEREO, 5000)[0] is False


def test_ignores_a_false_capture_pattern_and_other_streams():
    fake = b"OggS\x00" + bytes(40)                          # fails its CRC
    found, dropped, seq, _ = scan(fake + STEREO[PAGE_AT[2]:], 1500)
    assert found and seq == 3 and dropped == len(fake) + PAGE_AT[3] - PAGE_AT[2]
    serial, pre_skip = serial_and_preskip()
    assert wh.ogg_seek_scan(STEREO, serial + 1, pre_skip, 0, 512)[0] is False


def test_spliced_stream_is_valid_ogg():
    # What the decoder is given after a 1.5 s seek: the renumbered header,
    # then the file from page 3. Page numbers run on and every CRC holds.
    _, dropped, seq, _ = scan(STEREO[1000:], 1500)
    spliced = wh.renumber_ogg_header(STEREO, seq) + STEREO[1000 + dropped:]
    p, expect = 0, seq - 2
    while p < len(spliced):
        status, _, _, s, _, length = wh.read_ogg_page(spliced[p:])
        assert status == "ok" and s == expect
        p, expect = p + length, expect + 1
