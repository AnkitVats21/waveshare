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
DEFAULT_RADIO_URL = "http://rd.shalombeatsradio.com:7090/stream"

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