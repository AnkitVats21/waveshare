import socket
import time
import subprocess
import struct
import sys
import signal

# Configuration
ESP32_IP = "192.168.1.19"  # Update this to your ESP32's IP
UDP_PORT = 5006
SAMPLE_RATE = 16000  # 32kHz for better quality
CHUNK_SAMPLES = 640  # 20ms @ 32kHz
CHUNK_SIZE = CHUNK_SAMPLES * 2

# Default radio stream (change to your preferred M3U8 radio)
radio_url_list = {
    "bigfmradio": "https://stream-280.zeno.fm/dbstwo3dvhhtv?zt=eyJhbGciOiJIUzI1NiJ9.eyJzdHJlYW0iOiJkYnN0d28zZHZoaHR2IiwiaG9zdCI6InN0cmVhbS0yODAuemVuby5mbSIsInJ0dGwiOjUsImp0aSI6ImJLZVgxYnNqVFN5OTZqZHpqVVhXeXciLCJpYXQiOjE3Nzg5MjYxMDUsImV4cCI6MTc3ODkyNjE2NX0.oE4T0JPH1mSd3l-y8VgRMJVa4tGBp5Px1Ri94ItG2Gc",
    "radiocity":"",
    "hungama":"https://stream-289.zeno.fm/rm4i9pdex3cuv?zt=eyJhbGciOiJIUzI1NiJ9.eyJzdHJlYW0iOiJybTRpOXBkZXgzY3V2IiwiaG9zdCI6InN0cmVhbS0yODkuemVuby5mbSIsInJ0dGwiOjUsImp0aSI6IjQ5STFzSGpZUWJPS2tERG5ab0g4aEEiLCJpYXQiOjE3Nzg5MjYzOTUsImV4cCI6MTc3ODkyNjQ1NX0.9xddPetOxmI9g5LZe3G97FqO5MURBsl1jwGSRCj53Ms",
    "redfm":"https://stream-175.zeno.fm/9phrkb1e3v8uv?zt=eyJhbGciOiJIUzI1NiJ9.eyJzdHJlYW0iOiI5cGhya2IxZTN2OHV2IiwiaG9zdCI6InN0cmVhbS0xNzUuemVuby5mbSIsInJ0dGwiOjUsImp0aSI6IkFSYWVKQjZ6VF9PZ3h6eG5wTVpYX2ciLCJpYXQiOjE3Nzg5MjY0ODksImV4cCI6MTc3ODkyNjU0OX0.kjx8bh_lvIAMbwqFcD9RbV_0wm2nUjqMGgVnM1j9OWg",

}
DEFAULT_RADIO_URL = radio_url_list["redfm"]
def main():
    target_ip = sys.argv[1] if len(sys.argv) > 1 else ESP32_IP
    radio_url = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_RADIO_URL

    print(f"--- RTP Audio Streamer for M3U8 Radio ---")
    print(f"Target: {target_ip}:{UDP_PORT}")
    print(f"Radio Stream: {radio_url}")
    print(f"Format: PCM S16LE @ {SAMPLE_RATE}Hz Mono")
    print("Streaming started. Press Ctrl+C to stop.")

    # ffmpeg command to fetch and decode M3U8 stream
    # -re: real-time playback rate
    # -i: input URL (supports M3U8/HLS)
    # Additional flags for HLS reliability
    command = [
        'ffmpeg',
        '-re',                          # Real-time rate
        '-i', radio_url,                # M3U8 radio URL
        '-f', 's16le',                  # Raw PCM format
        '-acodec', 'pcm_s16le',         # PCM codec
        '-ac', '1',                     # Mono channel
        '-ar', str(SAMPLE_RATE),        # Sample rate
        '-'
    ]

    try:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except FileNotFoundError:
        print("Error: 'ffmpeg' not found. Please install ffmpeg.")
        print("Install on Ubuntu/Debian: sudo apt install ffmpeg")
        print("Install on MacOS: brew install ffmpeg")
        return

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    seq_num = 0
    timestamp = 0
    ssrc = 0x12345678

    # Handle Ctrl+C gracefully
    def signal_handler(sig, frame):
        print("\nStopping stream...")
        process.terminate()
        sock.close()
        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)

    try:
        while True:
            # Read exactly one chunk of audio (20ms)
            raw_data = process.stdout.read(CHUNK_SIZE)

            if not raw_data:
                print("Stream ended or buffer underrun. Reconnecting...")
                # Wait a bit and restart ffmpeg
                time.sleep(2)
                process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                continue

            # RTP Header
            # V=2, P=0, X=0, CC=0, M=0, PT=96 (dynamic PCM)
            header = struct.pack('!BBHII',
                                 0x80,      # V=2, P=0, X=0, CC=0
                                 96,        # Payload type (dynamic)
                                 seq_num,
                                 timestamp,
                                 ssrc)

            # Send RTP Packet via UDP
            sock.sendto(header + raw_data, (target_ip, UDP_PORT))

            # Update sequence number and timestamp
            seq_num = (seq_num + 1) & 0xFFFF
            timestamp = (timestamp + CHUNK_SAMPLES) & 0xFFFFFFFF

            # Small sleep to prevent overwhelming the network
            # The '-re' flag in ffmpeg already handles timing,
            # but this helps with network pacing
            time.sleep(0.001)  # 1ms delay

    except KeyboardInterrupt:
        print("\nStreaming stopped.")
    except Exception as e:
        print(f"Error: {e}")
    finally:
        process.terminate()
        sock.close()
        print("Done.")

if __name__ == "__main__":
    main()
