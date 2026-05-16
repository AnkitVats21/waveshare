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

SAMPLE_RATE = 16000
CHUNK_SAMPLES = 640          # 40ms @ 16kHz
CHUNK_SIZE = CHUNK_SAMPLES * 2

# ============================================================
# CHANNEL MANAGER
# ============================================================

class ChannelManager:
    def __init__(self):
        self.channels = {
            "bigfmradio":
                "https://stream-280.zeno.fm/dbstwo3dvhhtv?zt=eyJhbGciOiJIUzI1NiJ9.eyJzdHJlYW0iOiJkYnN0d28zZHZoaHR2IiwiaG9zdCI6InN0cmVhbS0yODAuemVuby5mbSIsInJ0dGwiOjUsImp0aSI6ImJLZVgxYnNqVFN5OTZqZHpqVVhXeXciLCJpYXQiOjE3Nzg5MjYxMDUsImV4cCI6MTc3ODkyNjE2NX0.oE4T0JPH1mSd3l-y8VgRMJVa4tGBp5Px1Ri94ItG2Gc",

            "hungama":
                "https://stream-289.zeno.fm/rm4i9pdex3cuv?zt=eyJhbGciOiJIUzI1NiJ9.eyJzdHJlYW0iOiJybTRpOXBkZXgzY3V2IiwiaG9zdCI6InN0cmVhbS0yODkuemVuby5mbSIsInJ0dGwiOjUsImp0aSI6IjQ5STFzSGpZUWJPS2tERG5ab0g4aEEiLCJpYXQiOjE3Nzg5MjYzOTUsImV4cCI6MTc3ODkyNjQ1NX0.9xddPetOxmI9g5LZe3G97FqO5MURBsl1jwGSRCj53Ms",

            "redfm":
                "https://stream-175.zeno.fm/9phrkb1e3v8uv?zt=eyJhbGciOiJIUzI1NiJ9.eyJzdHJlYW0iOiI5cGhya2IxZTN2OHV2IiwiaG9zdCI6InN0cmVhbS0xNzUuemVuby5mbSIsInJ0dGwiOjUsImp0aSI6IkFSYWVKQjZ6VF9PZ3h6eG5wTVpYX2ciLCJpYXQiOjE3Nzg5MjY0ODksImV4cCI6MTc3ODkyNjU0OX0.kjx8bh_lvIAMbwqFcD9RbV_0wm2nUjqMGgVnM1j9OWg",
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

        self.running = True

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

        print(f"\n[STREAM] Starting channel:")
        print(f"URL: {url}\n")

        self.process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL
        )

    # --------------------------------------------------------

    def stop_ffmpeg(self):

        if not self.process:
            return

        try:
            self.process.terminate()
            self.process.wait(timeout=2)
        except Exception:
            self.process.kill()

        self.process = None

    # --------------------------------------------------------

    def create_rtp_header(self):

        header = struct.pack(
            '!BBHII',
            0x80,              # RTP Version 2
            96,                # Payload Type
            self.seq_num,
            self.timestamp,
            self.ssrc
        )

        return header

    # --------------------------------------------------------

    def send_audio_packet(self, raw_data):

        header = self.create_rtp_header()

        self.sock.sendto(
            header + raw_data,
            (self.target_ip, self.port)
        )

        self.seq_num = (self.seq_num + 1) & 0xFFFF
        self.timestamp = (
            self.timestamp + CHUNK_SAMPLES
        ) & 0xFFFFFFFF

    # --------------------------------------------------------

    def restart_if_channel_changed(self):

        new_channel = self.channel_manager.get_current_channel()

        if new_channel != self.current_channel:

            self.current_channel = new_channel

            url = self.channel_manager.get_current_url()

            print(f"\n[CHANNEL] Switched to: {new_channel}")

            self.start_ffmpeg(url)

    # --------------------------------------------------------

    def stream_loop(self):

        while self.running:

            self.restart_if_channel_changed()

            if not self.process:
                time.sleep(1)
                continue

            raw_data = self.process.stdout.read(CHUNK_SIZE)

            if not raw_data:
                print("[WARN] Stream ended. Reconnecting...")
                time.sleep(2)

                url = self.channel_manager.get_current_url()
                self.start_ffmpeg(url)

                continue

            self.send_audio_packet(raw_data)

            time.sleep(0.001)

    # --------------------------------------------------------

    def stop(self):

        self.running = False

        self.stop_ffmpeg()

        self.sock.close()


# ============================================================
# CONSOLE CONTROL
# ============================================================

def console_loop(channel_manager):

    help_text = """
================ COMMANDS ================

list
    Show available channels

switch <channel_name>
    Change current channel

add <name> <url>
    Add new radio channel

current
    Show current channel

help
    Show commands

exit
    Exit application

==========================================
"""

    print(help_text)

    while True:

        try:
            cmd = input("radio> ").strip()

            if not cmd:
                continue

            parts = cmd.split(maxsplit=2)

            # --------------------------------

            if parts[0] == "list":

                print("\nAvailable Channels:\n")

                for ch in channel_manager.list_channels():
                    print(f" - {ch}")

                print()

            # --------------------------------

            elif parts[0] == "switch":

                if len(parts) < 2:
                    print("Usage: switch <channel_name>")
                    continue

                channel = parts[1]

                if channel_manager.change_channel(channel):
                    print(f"Switched to: {channel}")
                else:
                    print("Channel not found.")

            # --------------------------------

            elif parts[0] == "add":

                if len(parts) < 3:
                    print("Usage: add <name> <url>")
                    continue

                name = parts[1]
                url = parts[2]

                channel_manager.add_channel(name, url)

                print(f"Added channel: {name}")

            # --------------------------------

            elif parts[0] == "current":

                print(
                    f"Current channel: "
                    f"{channel_manager.get_current_channel()}"
                )

            # --------------------------------

            elif parts[0] == "help":

                print(help_text)

            # --------------------------------

            elif parts[0] == "exit":

                break

            # --------------------------------

            else:
                print("Unknown command")

        except EOFError:
            break
        except KeyboardInterrupt:
            break


# ============================================================
# MAIN
# ============================================================

def main():

    target_ip = (
        sys.argv[1]
        if len(sys.argv) > 1
        else ESP32_IP
    )

    print("\n=== RTP RADIO STREAMER ===")
    print(f"Target: {target_ip}:{UDP_PORT}")
    print(f"PCM: S16LE {SAMPLE_RATE}Hz Mono\n")

    channel_manager = ChannelManager()

    streamer = RTPRadioStreamer(
        target_ip,
        UDP_PORT,
        channel_manager
    )

    # --------------------------------------------------------

    def signal_handler(sig, frame):

        print("\nStopping streamer...")

        streamer.stop()

        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)

    # --------------------------------------------------------

    stream_thread = threading.Thread(
        target=streamer.stream_loop,
        daemon=True
    )

    stream_thread.start()

    # Interactive console
    console_loop(channel_manager)

    # Cleanup
    streamer.stop()

    print("Done.")


# ============================================================

if __name__ == "__main__":
    main()