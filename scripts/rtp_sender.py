# import socket
# import sys
# import subprocess
# import time
# import struct

# # Configuration
# ESP32_IP = "192.168.1.19" # Update this to your ESP32's IP
# UDP_PORT = 5006
# SAMPLE_RATE = 32000
# CHUNK_SAMPLES = 640 # 20ms @ 32kHz
# CHUNK_SIZE = CHUNK_SAMPLES * 2

# # Example Radio URL
# DEFAULT_SOURCE = "http://stream.live.vc.bbc.co.uk/bbc_world_service"

# def main():
#     target_ip = sys.argv[1] if len(sys.argv) > 1 else ESP32_IP
#     input_source = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_SOURCE
    
#     print(f"--- Professional RTP Audio Streamer (32kHz) ---")
#     print(f"Target: {target_ip}:{UDP_PORT}")
#     print(f"Source: {input_source}")
#     print(f"Format: PCM S16LE @ {SAMPLE_RATE}Hz Mono")

#     # ffmpeg command
#     # -re ensures it reads at real-time speed
#     command = [
#         'ffmpeg',
#         '-re',                     
#         '-i', input_source,        
#         '-f', 's16le',             
#         '-acodec', 'pcm_s16le',
#         '-ac', '1',                
#         '-ar', str(SAMPLE_RATE),   
#         '-'                        
#     ]
    
#     try:
#         process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
#     except FileNotFoundError:
#         print("Error: 'ffmpeg' not found. Please install ffmpeg.")
#         return

#     sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
#     seq_num = 0
#     timestamp = 0
#     ssrc = 0x12345678

#     print("Streaming started. Press Ctrl+C to stop.")

#     try:
#         while True:
#             # Read exactly one 20ms chunk
#             payload = process.stdout.read(CHUNK_SIZE)
#             if not payload:
#                 print("Stream ended.")
#                 break

#             # RTP Header (using your struct suggestion)
#             # Payload Type 96 is for Dynamic PCM
#             header = struct.pack('!BBHII', 0x80, 96, seq_num, timestamp, ssrc)

#             # Send packet
#             sock.sendto(header + payload, (target_ip, UDP_PORT))

#             seq_num = (seq_num + 1) & 0xFFFF
#             timestamp = (timestamp + CHUNK_SAMPLES) & 0xFFFFFFFF
            
#     except KeyboardInterrupt:
#         print("\nStreaming stopped.")
#     finally:
#         process.terminate()
#         sock.close()

# if __name__ == "__main__":
#     main()

import socket
import time
import subprocess
import struct
import sys

# Configuration - Default to common settings
ESP32_IP = "192.168.1.19" # Update this to your ESP32's IP
UDP_PORT = 5006
AUDIO_FILE = "khat.mp3"
SAMPLE_RATE = 16000
CHUNK_SAMPLES = 640 # 20ms @ 32kHz
CHUNK_SIZE = CHUNK_SAMPLES * 2
USE_COMPRESSION = False # Set to True for G.711 u-law (halves bandwidth)

def main():
    target_ip = sys.argv[1] if len(sys.argv) > 1 else ESP32_IP
    
    # Adjust for compression
    global CHUNK_SIZE
    if USE_COMPRESSION:
        CHUNK_SIZE = CHUNK_SAMPLES # 1 byte per sample in u-law
        format_cmd = ['-f', 'mulaw', '-acodec', 'pcm_mulaw']
    else:
        CHUNK_SIZE = CHUNK_SAMPLES * 2 # 2 bytes per sample in PCM
        format_cmd = ['-f', 's16le', '-acodec', 'pcm_s16le']

    print(f"--- RTP Audio Sender ---")
    print(f"Target: {target_ip}:{UDP_PORT}")
    print(f"Format: {'G.711 u-law' if USE_COMPRESSION else 'PCM S16LE'} @ {SAMPLE_RATE}Hz")

    # Start ffmpeg
    command = [
        'ffmpeg',
        '-re',
        '-i', AUDIO_FILE,
        *format_cmd,
        '-ac', '1',
        '-ar', str(SAMPLE_RATE),
        '-'
    ]
    
    try:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except FileNotFoundError:
        print("Error: 'ffmpeg' not found. Please install ffmpeg.")
        return

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    seq_num = 0
    timestamp = 0
    ssrc = 0x12345678

    print("Streaming started. Press Ctrl+C to stop.")

    try:
        while True:
            # Read exactly one chunk of audio
            raw_data = process.stdout.read(CHUNK_SIZE)
            if not raw_data:
                break

            # RTP Header (12 bytes)
            header = struct.pack('!BBHII', 
                                 0x80, 96, seq_num, timestamp, ssrc)
            
            # Send RTP Packet
            sock.sendto(header + raw_data, (target_ip, UDP_PORT))

            seq_num = (seq_num + 1) & 0xFFFF
            timestamp += CHUNK_SAMPLES
            
            # Note: ffmpeg '-re' handles the timing, but we can add a tiny sleep 
            # if needed to prevent network bursts.
    except KeyboardInterrupt:
        print("\nStreaming stopped.")
    finally:
        process.terminate()
        sock.close()
        print("Done.")

if __name__ == "__main__":
    main()
