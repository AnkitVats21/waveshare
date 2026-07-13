#!/usr/bin/env python3
import os
import sys
import time
import socket
import urllib.parse
from http.server import HTTPServer, BaseHTTPRequestHandler

# Default configuration
PORT = 8000
CHUNK_SIZE = 4096  # 4KB chunk sizes (matching common network buffers)
DEFAULT_SONGS_DIR = "/home/ankitm/Music/songs/"

def get_local_ip():
    """Helper to get the local IP address for printing MQTT payloads."""
    try:
        # Connect to a dummy external address to resolve local IP
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"

class ChunkedMediaHTTPHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        # Override to print cleanly to stderr
        sys.stderr.write(f"[{self.log_date_time_string()}] {format % args}\n")

    def do_GET(self):
        parsed_url = urllib.parse.urlparse(self.path)
        path = parsed_url.path
        query = urllib.parse.parse_qs(parsed_url.query)

        # Handle health check
        if path == "/health":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"OK")
            return

        # Extract song_id from query parameters or path URL segment
        # e.g., /stream?song_id=my_song or /songs/my_song
        song_id = None
        if "song_id" in query:
            song_id = query["song_id"][0]
        elif path.startswith("/songs/"):
            song_id = path.split("/songs/")[1]
        elif path.startswith("/stream/"):
            song_id = path.split("/stream/")[1]

        if song_id:
            song_id = song_id.strip().strip("'\"<>")

        if not song_id:
            # Fallback check if path itself is /stream
            if path == "/stream" or path == "/song":
                self.send_error(400, "Bad Request: Missing song_id parameter")
            else:
                self.send_error(404, "Not Found: Use /stream?song_id=xxxx or /songs/xxxx")
            return

        # Locate song file
        target_file = self.find_audio_file(song_id)
        if not target_file:
            self.send_error(404, f"Song '{song_id}' not found. Place audio files (.opus/.ogg/.wav) in: {DEFAULT_SONGS_DIR}")
            return

        # Serve the file
        try:
            self.serve_file(target_file)
        except ConnectionError:
            print(f"Client disconnected during streaming of: {song_id}")
        except Exception as e:
            print(f"Error serving file: {e}")

    def find_audio_file(self, song_id):
        # Support common audio file extensions
        extensions = [".opus", ".ogg", ".wav"]
        search_dirs = [DEFAULT_SONGS_DIR, os.getcwd(), os.path.dirname(DEFAULT_SONGS_DIR)]

        # If absolute path or relative path exists directly
        if os.path.isfile(song_id):
            return song_id

        for directory in search_dirs:
            if not os.path.exists(directory):
                continue
            for ext in extensions:
                candidate = os.path.join(directory, f"{song_id}{ext}")
                if os.path.isfile(candidate):
                    return candidate
                # Also check direct filename match in case extension was included in song_id
                candidate_direct = os.path.join(directory, song_id)
                if os.path.isfile(candidate_direct):
                    return candidate_direct
        return None

    def serve_file(self, filepath):
        file_size = os.path.getsize(filepath)
        filename = os.path.basename(filepath)
        
        # Determine content type
        content_type = "application/octet-stream"
        if filepath.endswith(".opus"):
            content_type = "audio/opus"
        elif filepath.endswith(".ogg"):
            content_type = "audio/ogg"
        elif filepath.endswith(".wav"):
            content_type = "audio/wav"

        print(f"Streaming file: {filename} ({file_size} bytes) as {content_type}")

        # Send HTTP Headers
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(file_size))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()

        # Stream file chunk by chunk
        start_time = time.time()
        bytes_sent = 0
        
        # Optional: Simulate network speed limit (e.g. 64 KB/s to test buffer stability)
        # Set to None for maximum speed.
        rate_limit_kbps = None 

        with open(filepath, "rb") as f:
            while True:
                chunk = f.read(CHUNK_SIZE)
                if not chunk:
                    break
                
                self.wfile.write(chunk)
                bytes_sent += len(chunk)

                # Rate limit pacing
                if rate_limit_kbps:
                    expected_time = bytes_sent / (rate_limit_kbps * 1024)
                    elapsed_time = time.time() - start_time
                    if elapsed_time < expected_time:
                        time.sleep(expected_time - elapsed_time)

        duration = time.time() - start_time
        print(f"Finished streaming: {filename}. Sent {bytes_sent} bytes in {duration:.2f} seconds.")

def run_server():
    local_ip = get_local_ip()
    
    # Ensure songs directory exists
    os.makedirs(DEFAULT_SONGS_DIR, exist_ok=True)

    print("==========================================================")
    print("                WAVESHARE TEST MEDIA SERVER               ")
    print("==========================================================")
    print(f"Local IP Address: {local_ip}")
    print(f"Port:             {PORT}")
    print(f"Songs Directory:  {DEFAULT_SONGS_DIR}")
    print("==========================================================")
    print("\nHow to play a song:")
    print("1. Place an .opus, .ogg, or .wav file in the songs directory.")
    print("   Example file: recordings/test_recording.wav (already exists!)")
    print("\n2. Publish an MQTT command to play the song:")
    print(f"   Topic:   device/waveshare/media")
    print(f'   Payload: {{"song_id": "test_recording", "song_url": "http://{local_ip}:{PORT}/stream?song_id=test_recording"}}')
    print("\nServer is starting...")

    server_address = ("", PORT)
    httpd = HTTPServer(server_address, ChunkedMediaHTTPHandler)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping media server.")
        httpd.server_close()
        sys.exit(0)

if __name__ == "__main__":
    if len(sys.argv) > 1:
        try:
            PORT = int(sys.argv[1])
        except ValueError:
            print(f"Invalid port: {sys.argv[1]}. Using default port 8000.")
    run_server()
