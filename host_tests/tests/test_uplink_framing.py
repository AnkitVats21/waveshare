import random
import pytest
import waveshare_host as wh


def test_accumulation_single_chunk():
    acc = wh.PcmAccumulator()
    # 2 full frames (2 * 640 bytes = 1280 bytes)
    data = bytes([i % 256 for i in range(1280)])
    frames = acc.feed(data)
    assert len(frames) == 2
    assert frames[0] == data[:640]
    assert frames[1] == data[640:1280]
    assert acc.remainder_bytes() == 0
    assert acc.remainder_samples() == 0


def test_accumulation_partial_and_remainder():
    acc = wh.PcmAccumulator()
    # 200 bytes: no full frame
    frames = acc.feed(b"A" * 200)
    assert len(frames) == 0
    assert acc.remainder_bytes() == 200

    # 500 more bytes: total 700 bytes -> 1 frame (640 bytes), 60 bytes remainder
    frames = acc.feed(b"B" * 500)
    assert len(frames) == 1
    assert frames[0] == (b"A" * 200) + (b"B" * 440)
    assert acc.remainder_bytes() == 60
    assert acc.flush_bytes() == b"B" * 60
    assert acc.remainder_bytes() == 0


def test_random_chunks_match_big_chunk():
    """Test that random chunk sizes produce the exact same frame sequence as one big chunk."""
    rng = random.Random(42)
    # 25,000 bytes: 39 frames of 640 bytes = 24,960 bytes, remainder 40 bytes
    pcm_data = bytes([rng.randint(0, 255) for _ in range(25000)])

    # 1. Baseline: process all at once
    acc_big = wh.PcmAccumulator()
    frames_big = acc_big.feed(pcm_data)
    rem_big = acc_big.flush_bytes()

    assert len(frames_big) == 39
    assert len(rem_big) == 40

    # 2. Slice into random chunk sizes across multiple test runs
    for run in range(5):
        acc_random = wh.PcmAccumulator()
        frames_random = []
        offset = 0
        while offset < len(pcm_data):
            # random chunk size between 1 and 1500 bytes
            chunk_size = rng.randint(1, 1500)
            chunk = pcm_data[offset : offset + chunk_size]
            offset += len(chunk)
            frames_random.extend(acc_random.feed(chunk))

        rem_random = acc_random.flush_bytes()

        assert len(frames_random) == len(frames_big)
        assert frames_random == frames_big
        assert rem_random == rem_big


def test_frame_builder_roundtrip():
    """Test that packing Opus packets into a frame round-trips through parser."""
    builder = wh.OpusFrameBuilder(wh.MIC_FRAME_MAX)
    assert builder.empty()
    assert builder.size() == 2
    assert builder.packet_count() == 0

    p1 = b"\x01\x02\x03\x04"
    p2 = b"Hello, Opus packet!" * 5
    p3 = bytes([i % 256 for i in range(120)])

    assert builder.add_packet(p1)
    assert builder.add_packet(p2)
    assert builder.add_packet(p3)

    assert not builder.empty()
    assert builder.packet_count() == 3

    frame = builder.data()
    # Header: channel 1, flag 0x02
    assert frame[0] == wh.CHANNEL_MIC
    assert frame[1] == wh.FLAG_OPUS

    # Parse frame
    parsed = wh.parse_opus_frame(frame)
    assert parsed is not None
    assert len(parsed) == 3
    assert parsed[0] == p1
    assert parsed[1] == p2
    assert parsed[2] == p3


def test_frame_builder_size_limit():
    """Test that frame builder strictly respects capacity limits."""
    # Capacity 40: header (2) + pkt1 (2+15=17) + pkt2 (2+15=17) = 36 bytes.
    # A 3rd packet of 5 bytes needs 2+5=7 bytes -> 36+7=43 > 40 -> must return False.
    builder = wh.OpusFrameBuilder(40)
    pkt1 = b"1" * 15
    pkt2 = b"2" * 15
    pkt3 = b"3" * 5

    assert builder.add_packet(pkt1)
    assert builder.add_packet(pkt2)
    assert not builder.add_packet(pkt3)

    assert builder.packet_count() == 2
    assert builder.size() == 2 + (2 + 15) + (2 + 15)

    # Frame should parse cleanly with only the 2 accepted packets
    parsed = wh.parse_opus_frame(builder.data())
    assert parsed == [pkt1, pkt2]


def test_pcm_fallback_flush():
    """Test flushing partial remainder as a PCM frame on fallback."""
    acc = wh.PcmAccumulator()
    # Feed 300 bytes (less than 640)
    acc.feed(b"\x12\x34" * 150)
    assert acc.remainder_bytes() == 300

    flushed = acc.flush_bytes()
    assert len(flushed) == 300
    assert acc.remainder_bytes() == 0

    pcm_frame = wh.build_pcm_frame(flushed)
    assert pcm_frame is not None
    assert len(pcm_frame) == 302
    assert pcm_frame[0] == wh.CHANNEL_MIC
    assert pcm_frame[1] == wh.FLAG_PCM

    # Verify parse_pcm_frame
    payload = wh.parse_pcm_frame(pcm_frame)
    assert payload == flushed


def test_malformed_opus_frames():
    """Verify robust rejection of malformed frames."""
    # Frame too short
    assert wh.parse_opus_frame(b"") is None
    assert wh.parse_opus_frame(b"\x01") is None

    # Wrong channel
    assert wh.parse_opus_frame(b"\x02\x02") is None

    # Flags indicate PCM, not Opus
    assert wh.parse_opus_frame(b"\x01\x00\x05\x00abcde") is None

    # Truncated length prefix: says 1 packet but only 1 byte length
    assert wh.parse_opus_frame(b"\x01\x02\x05") is None

    # Truncated packet payload: length header says 10 bytes, only 3 present
    assert wh.parse_opus_frame(b"\x01\x02\x0a\x00abc") is None
