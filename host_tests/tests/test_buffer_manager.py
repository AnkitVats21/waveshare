import threading
import time
import pytest
import waveshare_host as wh


def test_buffer_registration_and_size(buffer_manager):
    """Test buffer descriptor registration and PSRAM allocation."""
    buf_size = 4096
    buf_id = wh.register_buffer("test_pcm_buf", buf_size, 2)  # 2 = BYTEBUF
    assert buf_id != 0xFF
    assert buffer_manager.init_all() is True
    assert buffer_manager.size(buf_id) == buf_size
    assert buffer_manager.get_used_bytes(buf_id) == 0


def test_byte_buffer_partial_reads(buffer_manager):
    """Test byte stream chunked writes and partial reads with receive(max_bytes)."""
    buf_size = 2048
    buf_id = wh.register_buffer("test_chunk_buf", buf_size, 2)
    buffer_manager.init_all()

    test_payload = bytes([i % 256 for i in range(500)])
    assert buffer_manager.send(buf_id, test_payload, 0) is True
    assert buffer_manager.get_used_bytes(buf_id) == 500

    # Read first 200 bytes
    chunk1 = buffer_manager.receive(buf_id, max_bytes=200, timeout_ms=0)
    assert len(chunk1) == 200
    assert chunk1 == test_payload[:200]
    assert buffer_manager.get_used_bytes(buf_id) == 300

    # Read next 300 bytes
    chunk2 = buffer_manager.receive(buf_id, max_bytes=300, timeout_ms=0)
    assert len(chunk2) == 300
    assert chunk2 == test_payload[200:500]
    assert buffer_manager.get_used_bytes(buf_id) == 0


def test_buffer_drop_on_full(buffer_manager):
    """Test that send() fails gracefully and drops are tracked when buffer is full."""
    buf_size = 512
    buf_id = wh.register_buffer("test_small_buf", buf_size, 2)
    buffer_manager.init_all()

    # Fill completely
    data = b"X" * 512
    assert buffer_manager.send(buf_id, data, 0) is True
    assert buffer_manager.get_used_bytes(buf_id) == 512

    # Attempt send when full (timeout=0: non-blocking drop-on-full)
    overflow_data = b"DROPPED"
    assert buffer_manager.send(buf_id, overflow_data, timeout_ms=0) is False

    # Verify original data intact
    read_back = buffer_manager.receive(buf_id, max_bytes=512, timeout_ms=0)
    assert read_back == data


def test_buffer_flush(buffer_manager):
    """Test non-blocking flush drains all remaining bytes immediately."""
    buf_size = 1024
    buf_id = wh.register_buffer("test_flush_buf", buf_size, 2)
    buffer_manager.init_all()

    buffer_manager.send(buf_id, b"DATA_BEFORE_FLUSH" * 10, 0)
    assert buffer_manager.get_used_bytes(buf_id) > 0

    buffer_manager.flush(buf_id)
    assert buffer_manager.get_used_bytes(buf_id) == 0

    # Reading should return empty
    empty_read = buffer_manager.receive(buf_id, max_bytes=100, timeout_ms=0)
    assert len(empty_read) == 0


def test_buffer_producer_consumer_concurrency(buffer_manager):
    """Multi-threaded audio stream simulation: producer writes frames, consumer reads frames."""
    buf_size = 8192
    buf_id = wh.register_buffer("test_audio_stream", buf_size, 2)
    buffer_manager.init_all()

    total_frames = 200
    frame_size = 128
    sent_data = bytearray()
    received_data = bytearray()
    consumer_done = threading.Event()
    producer_error = []
    consumer_error = []

    def producer():
        nonlocal sent_data
        try:
            for i in range(total_frames):
                frame = bytes([(i + j) % 256 for j in range(frame_size)])
                sent_data.extend(frame)
                # Send with short timeout if temporarily constrained
                while not buffer_manager.send(buf_id, frame, timeout_ms=50):
                    time.sleep(0.001)
                time.sleep(0.0005)  # Simulate 16kHz audio sample rate pacing
        except Exception as e:
            producer_error.append(str(e))

    def consumer():
        nonlocal received_data
        try:
            while len(received_data) < total_frames * frame_size:
                chunk = buffer_manager.receive(buf_id, max_bytes=frame_size, timeout_ms=100)
                if chunk:
                    received_data.extend(chunk)
                else:
                    time.sleep(0.001)
        except Exception as e:
            consumer_error.append(str(e))
        finally:
            consumer_done.set()

    t_prod = threading.Thread(target=producer)
    t_cons = threading.Thread(target=consumer)

    t_cons.start()
    t_prod.start()

    t_prod.join(timeout=5.0)
    t_cons.join(timeout=5.0)

    assert len(producer_error) == 0, f"Producer error: {producer_error}"
    assert len(consumer_error) == 0, f"Consumer error: {consumer_error}"
    assert len(received_data) == len(sent_data)
    assert received_data == sent_data
