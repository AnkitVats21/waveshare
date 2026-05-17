import socket
import wave
import sys
import time
import struct
import os

# Configuration
UDP_IP = "0.0.0.0"
UDP_PORT = 5005
OUTPUT_FILE = "recordings/test_recording.wav"
SAMPLE_RATE = 16000
CHANNELS = 1

def main():
    # Make sure recordings directory exists
    os.makedirs(os.path.dirname(OUTPUT_FILE), exist_ok=True)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind((UDP_IP, UDP_PORT))
    except Exception as e:
        print(f"Error: Could not bind to port {UDP_PORT}. {e}")
        sys.exit(1)

    print(f"--- Optimized Voice-Gated RTP Receiver ---")
    print(f"Listening on: {UDP_IP}:{UDP_PORT}")
    print(f"Format: {SAMPLE_RATE}Hz, Mono, 16-bit Signed PCM")
    print(f"Saving to: {OUTPUT_FILE}")
    print(f"Press Ctrl+C to finish recording...")

    # Step 1: Flush any stale packets currently sitting in the OS UDP receive buffer
    print("Flushing OS UDP socket receive buffer of any stale/old packets...")
    sock.setblocking(False)
    flush_count = 0
    try:
        while True:
            _, _ = sock.recvfrom(2048)
            flush_count += 1
    except BlockingIOError:
        pass
    sock.setblocking(True)
    if flush_count > 0:
        print(f"Discarded {flush_count} stale/buffered packets from earlier streams.")

    first_packet = True
    expected_timestamp = None
    current_ssrc = None

    try:
        with wave.open(OUTPUT_FILE, 'wb') as wav_file:
            wav_file.setnchannels(CHANNELS)
            wav_file.setsampwidth(2) # 16-bit = 2 bytes
            wav_file.setframerate(SAMPLE_RATE)

            while True:
                # Read UDP Packet
                data, addr = sock.recvfrom(2048)

                # Validate standard RTP Version 2 (first byte must be exactly 0x80)
                if len(data) < 12 or data[0] != 0x80:
                    continue

                # Parse RTP Header (12 bytes)
                # !BBHII: big-endian, flags, PT, seq, timestamp, ssrc
                _, _, seq_num, timestamp, ssrc = struct.unpack('!BBHII', data[:12])
                payload = data[12:]
                samples_in_payload = len(payload) // 2

                # Detect stream restarts or changes in the sender source
                if current_ssrc is not None and current_ssrc != ssrc:
                    print(f"SSRC changed from {current_ssrc} to {ssrc}. Stream restarted!")
                    first_packet = True
                current_ssrc = ssrc

                if first_packet:
                    first_packet = False
                    print(f"Active stream started from {addr}! SSRC: {ssrc}. Recording voice...")
                    expected_timestamp = timestamp + samples_in_payload
                else:
                    # Check for gaps in RTP timestamps
                    if timestamp > expected_timestamp:
                        gap_samples = timestamp - expected_timestamp
                        gap_seconds = gap_samples / SAMPLE_RATE

                        # Jitter buffer threshold: Only pad minor packet losses (< 200ms)
                        # to prevent audio distortion. Skip padding large VAD gaps.
                        if gap_seconds > 0.2:
                            print(f"Stream resumed after {gap_seconds:.2f}s pause. Appending...")
                        else:
                            print(f"Minor jitter gap detected: {gap_seconds:.3f}s. Reconstructing silence...")
                            wav_file.writeframes(b'\x00' * (gap_samples * 2))

                    expected_timestamp = timestamp + samples_in_payload

                # Write active audio payload
                wav_file.writeframes(payload)

    except KeyboardInterrupt:
        print(f"\nRecording finished. File saved: {OUTPUT_FILE}")
    except Exception as e:
        print(f"An error occurred: {e}")
    finally:
        sock.close()

if __name__ == "__main__":
    main()
