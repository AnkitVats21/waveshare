"""parseWebmCues: the seek index at the start of a WebM file.

The fixtures are the first bytes of two YouTube audio files, up to the first
Cluster (headers and Cues, no audio). The expected cues were checked against
every Cluster in the full files: same byte positions, same timestamps.
"""

from pathlib import Path

import pytest
import waveshare_host as wh

DATA = Path(__file__).parent / "data"

# 7:50 track, 48 clusters.
CUES_4DS = [(0, 1079)] + [(10001 + 10000 * i, o) for i, o in enumerate([
    156506, 324542, 481269, 651265, 820727, 979361, 1138237, 1304872, 1472942,
    1643130, 1813208, 1967568, 2121197, 2279596, 2456082, 2629042, 2793751,
    2961213, 3126414, 3290419, 3458164, 3616324, 3771797, 3929534, 4088196,
    4253888, 4416405, 4578272, 4746187, 4907044, 5071855, 5237835, 5393537,
    5546978, 5701930, 5871668, 6037940, 6201156, 6361518, 6517992, 6676526,
    6831737, 6988109, 7144133, 7299678, 7454769, 7606292])]

# 3:37 track, 22 clusters.
CUES_2SU = [(0, 637)] + [(10001 + 10000 * i, o) for i, o in enumerate([
    167830, 354038, 537511, 709529, 884598, 1056458, 1231408, 1401592, 1572237,
    1741739, 1917181, 2089474, 2262352, 2436799, 2603112, 2772485, 2939484,
    3109485, 3281779, 3453125, 3623706])]

FIXTURES = [("4dsFQFCvVGU.head.webm", CUES_4DS), ("2SUwOgmvzK4.head.webm", CUES_2SU)]


# ── A minimal EBML writer for synthetic files ────────────────────────────────

def vint_size(n, width=8):
    return ((1 << (7 * width)) | n).to_bytes(width, "big")


UNKNOWN = b"\x01\xff\xff\xff\xff\xff\xff\xff"


def el(id_, payload, size=None):
    id_bytes = id_.to_bytes((id_.bit_length() + 7) // 8, "big")
    return id_bytes + (size if size is not None else vint_size(len(payload))) + payload


def uint(id_, v):
    return el(id_, v.to_bytes(max(1, (v.bit_length() + 7) // 8), "big"))


EBML_HEADER = el(0x1A45DFA3, el(0x4282, b"webm"))


def cue(time, pos):
    return el(0xBB, uint(0xB3, time) + el(0xB7, uint(0xF7, 1) + uint(0xF1, pos)))


def webm(*children, segment_size=None):
    body = b"".join(children)
    seg = el(0x18538067, body, UNKNOWN if segment_size is None else vint_size(segment_size))
    return EBML_HEADER + seg, len(EBML_HEADER) + 4 + 8   # file, Segment payload start


def info(scale=1000000):
    return el(0x1549A966, uint(0x2AD7B1, scale))


CLUSTER = el(0x1F43B675, uint(0xE7, 0))


def parse(data):
    return wh.parse_webm_cues(bytes(data))


# ── Real files ───────────────────────────────────────────────────────────────

@pytest.mark.parametrize("name,expected", FIXTURES)
def test_real_file_index_matches_every_cluster(name, expected):
    status, cues, _ = parse((DATA / name).read_bytes())
    assert status == "found"
    assert cues == expected


@pytest.mark.parametrize("name,expected", FIXTURES)
def test_every_shorter_prefix_asks_for_more(name, expected):
    head = (DATA / name).read_bytes()
    for n in range(len(head)):
        status, cues, need = parse(head[:n])
        assert status == "need_more", n
        assert need > n and cues == []


@pytest.mark.parametrize("name,expected", FIXTURES)
def test_following_need_reaches_the_index(name, expected):
    head = (DATA / name).read_bytes()
    n, reads = 0, 0
    while True:
        status, cues, need = parse(head[:n])
        if status == "found":
            break
        assert status == "need_more"
        n, reads = need, reads + 1
        assert reads < 10
    assert cues == expected


# ── Synthetic files ──────────────────────────────────────────────────────────

def test_cluster_before_any_cues_is_not_found():
    data, _ = webm(info(), CLUSTER)
    assert parse(data)[0] == "not_found"


def test_known_size_segment_without_cues_is_not_found():
    body = info()
    data, _ = webm(body, segment_size=len(body))
    assert parse(data)[0] == "not_found"


def test_empty_cues_is_not_found():
    data, _ = webm(info(), el(0x1C53BB6B, b""), CLUSTER)
    assert parse(data)[0] == "not_found"


def test_positions_are_from_the_segment_payload_start():
    data, seg = webm(info(), el(0x1C53BB6B, cue(0, 100) + cue(10000, 5000)), CLUSTER)
    assert parse(data)[1] == [(0, seg + 100), (10000, seg + 5000)]


def test_timecode_scale_converts_to_ms():
    # 500 us per unit: 20000 units = 10 s.
    data, seg = webm(info(500000), el(0x1C53BB6B, cue(0, 10) + cue(20000, 900)), CLUSTER)
    assert parse(data)[1] == [(0, seg + 10), (10000, seg + 900)]


def test_cues_in_the_same_cluster_collapse_to_one():
    data, seg = webm(el(0x1C53BB6B, cue(0, 10) + cue(20, 10) + cue(10000, 900)))
    assert parse(data)[1] == [(0, seg + 10), (10000, seg + 900)]


def test_out_of_order_cues_are_invalid():
    data, _ = webm(el(0x1C53BB6B, cue(0, 900) + cue(10000, 10)))
    assert parse(data)[0] == "invalid"
    data, _ = webm(el(0x1C53BB6B, cue(10000, 10) + cue(0, 900)))
    assert parse(data)[0] == "invalid"


def test_cue_without_cluster_position_is_invalid():
    point = el(0xBB, uint(0xB3, 0) + el(0xB7, uint(0xF7, 1)))
    data, _ = webm(el(0x1C53BB6B, point))
    assert parse(data)[0] == "invalid"


def test_child_overrunning_its_parent_is_invalid():
    # A CuePoint claiming 200 bytes inside a 20-byte Cues element.
    point = el(0xBB, cue(0, 10)[1 + 8:], size=vint_size(200))
    data, _ = webm(el(0x1C53BB6B, point))
    assert parse(data)[0] == "invalid"


def test_huge_header_element_is_invalid_not_a_huge_read():
    data, _ = webm(el(0x1C53BB6B, b"", size=vint_size(64 * 1024 * 1024)))
    assert parse(data)[0] == "invalid"


def test_too_many_cues_are_invalid():
    points = b"".join(cue(i * 10, i * 100) for i in range(wh.MAX_CUES + 1))
    data, _ = webm(el(0x1C53BB6B, points))
    assert parse(data)[0] == "invalid"


@pytest.mark.parametrize("data", [
    b"OggS\x00\x02" + bytes(40),                  # an Ogg file
    bytes(64),                                    # zeros
    bytes(range(256)),                            # noise
    EBML_HEADER + el(0x1549A966, b""),            # EBML without a Segment
])
def test_not_webm_is_invalid(data):
    assert parse(data)[0] == "invalid"


def test_empty_input_asks_for_the_first_header():
    status, _, need = parse(b"")
    assert status == "need_more" and need > 0


# ── cueAtOrBefore ────────────────────────────────────────────────────────────

@pytest.mark.parametrize("t,expected", [
    (0, (0, 1079)),
    (10000, (0, 1079)),              # just before the second cluster
    (10001, (10001, 156506)),        # exactly on it
    (97000, (90001, 1472942)),
    (10 ** 9, (470001, 7606292)),    # past the end: the last cluster
])
def test_cue_at_or_before(t, expected):
    assert wh.cue_at_or_before(CUES_4DS, t) == expected


def test_cue_before_the_first_is_the_first():
    assert wh.cue_at_or_before([(500, 10), (10500, 99)], 100) == (500, 10)
