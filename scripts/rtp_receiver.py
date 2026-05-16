import socket
import wave
import sys

# Configuration
UDP_IP = "0.0.0.0"
UDP_PORT = 5005
OUTPUT_FILE = "recordings/test_recording.wav"
SAMPLE_RATE = 32000
CHANNELS = 1

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind((UDP_IP, UDP_PORT))
    except Exception as e:
        print(f"Error: Could not bind to port {UDP_PORT}. {e}")
        sys.exit(1)

    print(f"--- Optimized RTP PCM Receiver ---")
    print(f"Listening on: {UDP_IP}:{UDP_PORT}")
    print(f"Format: {SAMPLE_RATE}Hz, Mono, 16-bit Signed PCM")
    print(f"Saving to: {OUTPUT_FILE}")
    print(f"Press Ctrl+C to finish recording...")

    try:
        with wave.open(OUTPUT_FILE, 'wb') as wav_file:
            wav_file.setnchannels(CHANNELS)
            wav_file.setsampwidth(2) # 16-bit = 2 bytes
            wav_file.setframerate(SAMPLE_RATE)

            while True:
                # Standard RTP packet is ~12 byte header + payload
                data, addr = sock.recvfrom(2048)
                
                if len(data) > 12:
                    # Strip 12-byte RTP header
                    payload = data[12:]
                    
                    # Direct write of PCM data
                    wav_file.writeframes(payload)
                    
    except KeyboardInterrupt:
        print(f"\nRecording finished. File saved: {OUTPUT_FILE}")
    except Exception as e:
        print(f"An error occurred: {e}")
    finally:
        sock.close()

if __name__ == "__main__":
    main()
