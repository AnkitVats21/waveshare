import socket
import time
import subprocess
import struct
import sys
import signal
import threading

# ============================================================
# CONFIG
# ============================================================

ESP32_IP = "192.168.1.19"
UDP_PORT = 5006

SAMPLE_RATE = 24000
CHUNK_SAMPLES = 640          # 40ms @ 16kHz
CHUNK_SIZE = CHUNK_SAMPLES * 2

# ============================================================
# CHANNEL MANAGER
# ============================================================

class ChannelManager:
    def __init__(self):
        self.channels = {
            "bfm":
                "https://stream-280.zeno.fm/dbstwo3dvhhtv?zt=eyJhbGciOiJIUzI1NiJ9.eyJzdHJlYW0iOiJkYnN0d28zZHZoaHR2IiwiaG9zdCI6InN0cmVhbS0yODAuemVuby5mbSIsInJ0dGwiOjUsImp0aSI6ImJLZVgxYnNqVFN5OTZqZHpqVVhXeXciLCJpYXQiOjE3Nzg5MjYxMDUsImV4cCI6MTc3ODkyNjE2NX0.oE4T0JPH1mSd3l-y8VgRMJVa4tGBp5Px1Ri94ItG2Gc",

            "hungama":
                "https://stream-289.zeno.fm/rm4i9pdex3cuv?zt=eyJhbGciOiJIUzI1NiJ9.eyJzdHJlYW0iOiJybTRpOXBkZXgzY3V2IiwiaG9zdCI6InN0cmVhbS0yODkuemVuby5mbSIsInJ0dGwiOjUsImp0aSI6IjQ5STFzSGpZUWJPS2tERG5ab0g4aEEiLCJpYXQiOjE3Nzg5MjYzOTUsImV4cCI6MTc3ODkyNjQ1NX0.9xddPetOxmI9g5LZe3G97FqO5MURBsl1jwGSRCj53Ms",

            "redfm":
                "https://stream-175.zeno.fm/9phrkb1e3v8uv?zt=eyJhbGciOiJIUzI1NiJ9.eyJzdHJlYW0iOiI5cGhya2IxZTN2OHV2IiwiaG9zdCI6InN0cmVhbS0xNzUuemVuby5mbSIsInJ0dGwiOjUsImp0aSI6IlI3cFMzb1FZUVVTXzA0WDByaHY2bFEiLCJpYXQiOjE3Nzg5MzQ3ODEsImV4cCI6MTc3ODkzNDg0MX0.MOPJn-UtY-OZPcXsZ3L2bTO02ohm9ORGP-22CpDdgAY",
        }

        self.current_channel = "hungama"
        self.lock = threading.Lock()

    def add_channel(self, name, url):
        with self.lock:
            self.channels[name] = url

    def change_channel(self, name):
        with self.lock:
            if name not in self.channels:
                return False
            self.current_channel = name
            return True

    def get_current_channel(self):
        with self.lock:
            return self.current_channel

    def get_current_url(self):
        with self.lock:
            return self.channels[self.current_channel]

    def list_channels(self):
        with self.lock:
            return list(self.channels.keys())


# ============================================================
# RTP STREAMER
# ============================================================

class RTPRadioStreamer:

    def __init__(self, target_ip, port, channel_manager):
        self.target_ip = target_ip
        self.port = port
        self.channel_manager = channel_manager

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        self.seq_num = 0
        self.timestamp = 0
        self.ssrc = 0x12345678

        self.running = False
        self.thread = None

        self.process = None
        self.current_channel = None

    # --------------------------------------------------------

    def build_ffmpeg_command(self, url):
        return [
            'ffmpeg',
            '-loglevel', 'quiet',
            '-re',
            '-i', url,
            '-f', 's16le',
            '-acodec', 'pcm_s16le',
            '-ac', '1',
            '-ar', str(SAMPLE_RATE),
            '-'
        ]

    # --------------------------------------------------------

    def start_ffmpeg(self, url):
        if self.process:
            self.stop_ffmpeg()

        command = self.build_ffmpeg_command(url)
        print(f"\n[STREAM] Connecting to: {url}")

        try:
            self.process = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                bufsize=CHUNK_SIZE * 2
            )
        except Exception as e:
            print(f"[ERROR] Failed to start ffmpeg: {e}")

    # --------------------------------------------------------

    def stop_ffmpeg(self):
        if not self.process:
            return

        try:
            self.process.terminate()
            self.process.wait(timeout=1)
        except Exception:
            try:
                self.process.kill()
            except:
                pass

        self.process = None

    # --------------------------------------------------------

    def create_rtp_header(self):
        return struct.pack(
            '!BBHII',
            0x80,              # RTP Version 2
            96,                # Payload Type (L16 usually)
            self.seq_num,
            self.timestamp,
            self.ssrc
        )

    # --------------------------------------------------------

    def send_audio_packet(self, raw_data):
        header = self.create_rtp_header()
        try:
            self.sock.sendto(header + raw_data, (self.target_ip, self.port))
            self.seq_num = (self.seq_num + 1) & 0xFFFF
            self.timestamp = (self.timestamp + CHUNK_SAMPLES) & 0xFFFFFFFF
        except Exception as e:
            pass # Socket errors shouldn't crash the streamer

    # --------------------------------------------------------

    def stream_loop(self):
        print("[STREAM] Thread started.")
        while self.running:
            # Check for channel changes
            new_channel = self.channel_manager.get_current_channel()
            if new_channel != self.current_channel:
                self.current_channel = new_channel
                url = self.channel_manager.get_current_url()
                print(f"\n[CHANNEL] Switched to: {new_channel}")
                self.start_ffmpeg(url)

            if not self.process:
                time.sleep(0.5)
                continue

            # This is a blocking read, but since ffmpeg uses -re,
            # it naturally paces the loop to ~20ms per CHUNK_SIZE.
            try:
                raw_data = self.process.stdout.read(CHUNK_SIZE)
            except Exception as e:
                print(f"[ERROR] Read error: {e}")
                raw_data = None

            if not raw_data:
                # If we get no data, the stream might have dropped
                if self.running:
                    print("[WARN] Stream ended or dropped. Reconnecting in 2s...")
                    time.sleep(2)
                    if self.current_channel:
                        url = self.channel_manager.get_current_url()
                        self.start_ffmpeg(url)
                continue

            self.send_audio_packet(raw_data)

        print("[STREAM] Thread exiting...")
        self.stop_ffmpeg()

    # --------------------------------------------------------

    def start(self):
        if self.running:
            return
        self.running = True
        self.thread = threading.Thread(target=self.stream_loop, daemon=True)
        self.thread.start()

    def stop(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=2)
        self.stop_ffmpeg()
        try:
            self.sock.close()
        except:
            pass


# ============================================================
# CONSOLE CONTROL
# ============================================================

def console_loop(channel_manager):
    help_text = """
================ COMMANDS ================
list              - Show available channels
switch <name>     - Change current channel
add <name> <url>  - Add new radio channel
current           - Show current channel
help              - Show commands
exit              - Exit application
==========================================
"""
    print(help_text)

    while True:
        try:
            # Use sys.stdout.write for the prompt to ensure it's visible
            sys.stdout.write("radio> ")
            sys.stdout.flush()

            line = sys.stdin.readline()
            if not line:
                break

            cmd = line.strip()
            if not cmd:
                continue

            parts = cmd.split(maxsplit=2)
            action = parts[0].lower()

            if action == "list":
                print("\nAvailable Channels:")
                for ch in channel_manager.list_channels():
                    print(f" - {ch}")
                print()

            elif action == "switch":
                if len(parts) < 2:
                    print("Usage: switch <channel_name>")
                    continue
                channel = parts[1]
                if channel_manager.change_channel(channel):
                    print(f"Switching to: {channel}...")
                else:
                    print(f"Channel '{channel}' not found.")

            elif action == "add":
                if len(parts) < 3:
                    print("Usage: add <name> <url>")
                    continue
                name, url = parts[1], parts[2]
                channel_manager.add_channel(name, url)
                print(f"Added channel: {name}")

            elif action == "current":
                print(f"Current channel: {channel_manager.get_current_channel()}")

            elif action == "help":
                print(help_text)

            elif action in ["exit", "quit"]:
                break

            else:
                print(f"Unknown command: {action}")

        except KeyboardInterrupt:
            print("\nUse 'exit' to quit.")
            continue
        except EOFError:
            break


# ============================================================
# MAIN
# ============================================================

def main():
    target_ip = sys.argv[1] if len(sys.argv) > 1 else ESP32_IP

    print("\n=== RTP RADIO STREAMER ===")
    print(f"Target: {target_ip}:{UDP_PORT}")
    print(f"Format: PCM S16LE {SAMPLE_RATE}Hz Mono\n")

    channel_manager = ChannelManager()
    streamer = RTPRadioStreamer(target_ip, UDP_PORT, channel_manager)

    # Signal handling for clean exit
    def signal_handler(sig, frame):
        print("\n[INFO] Shutdown signal received.")
        streamer.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)

    # Start streamer background thread
    streamer.start()

    # Enter interactive console (main thread)
    try:
        console_loop(channel_manager)
    finally:
        print("Cleaning up...")
        streamer.stop()
        print("Done.")


if __name__ == "__main__":
    main()



# ============================================================

if __name__ == "__main__":
    main()
