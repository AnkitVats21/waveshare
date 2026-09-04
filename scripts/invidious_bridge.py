#!/usr/bin/env python3
"""
Lightweight Invidious-Compatible API Bridge for Home Raspberry Pi / Local PC.
Implements standard Invidious REST API endpoints using yt-dlp on residential IP:
- GET /api/v1/search?q=<query>
- GET /api/v1/videos/<videoId>
- GET /api/v1/stats (health check)
- GET /stream/<videoId> (optional live stream proxy through residential IP)

Allows ESP32 (and verify_invidious_playback.py) to stream music without:
- Datacenter IP blocking (YouTube blocks cloud servers, but allows home IPs)
- MQTT relays
- Disk caching / file storage
"""

import os
import sys
import json
import time
import argparse
import subprocess
import urllib.parse
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn

import threading

PORT = 8088
CACHE = {}
CACHE_TTL = 3600  # 1 hour in-memory cache for resolved stream URLs
RESOLVING_EVENTS = {}
RESOLVING_LOCK = threading.Lock()

def get_ytdlp_bin():
    # Priority: user local bin, system bin, or sys.executable -m yt_dlp
    local_bin = os.path.expanduser("~/.local/bin/yt-dlp")
    if os.path.isfile(local_bin) and os.access(local_bin, os.X_OK):
        return [local_bin]
    if subprocess.call(["which", "yt-dlp"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0:
        return ["yt-dlp"]
    return [sys.executable, "-m", "yt_dlp"]

YTDLP_CMD = get_ytdlp_bin()

def search_youtube(query, max_results=5):
    cache_key = f"search:{query}:{max_results}"
    now = time.time()
    if cache_key in CACHE:
        res, timestamp = CACHE[cache_key]
        if now - timestamp < CACHE_TTL:
            return res

    cmd = YTDLP_CMD + [
        "--no-warnings",
        "--skip-download",
        "--flat-playlist",
        "--dump-json",
        f"ytsearch{max_results}:{query}"
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        results = []
        for line in proc.stdout.strip().split("\n"):
            if not line:
                continue
            try:
                item = json.loads(line)
                results.append({
                    "videoId": item.get("id"),
                    "title": item.get("title"),
                    "author": item.get("uploader", item.get("channel", "Unknown")),
                    "lengthSeconds": int(item.get("duration") or 0)
                })
            except Exception:
                pass
        CACHE[cache_key] = (results, now)
        return results
    except Exception as e:
        print(f"[ERROR] Search failed: {e}")
        return []

def resolve_video_formats(video_id):
    cache_key = f"video:{video_id}"
    now = time.time()
    if cache_key in CACHE:
        res, timestamp = CACHE[cache_key]
        if now - timestamp < CACHE_TTL:
            return res

    with RESOLVING_LOCK:
        if video_id in RESOLVING_EVENTS:
            evt = RESOLVING_EVENTS[video_id]
            is_initiator = False
        else:
            evt = threading.Event()
            RESOLVING_EVENTS[video_id] = evt
            is_initiator = True

    if not is_initiator:
        print(f"[INVIDIOUS BRIDGE] ... waiting for in-progress resolution of '{video_id}'")
        evt.wait(timeout=45)
        if cache_key in CACHE:
            return CACHE[cache_key][0]

    cmd = YTDLP_CMD + [
        "--no-warnings",
        "-f", "ba[ext=webm]/ba",
        "--print", "%(title)s",
        "--print", "%(url)s",
        "--print", "%(abr)s",
        f"https://www.youtube.com/watch?v={video_id}"
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=40)
        lines = [l.strip() for l in proc.stdout.strip().split("\n") if l.strip()]
        if len(lines) >= 2:
            title = lines[0]
            stream_url = lines[1]
            try:
                abr = float(lines[2]) if len(lines) > 2 else 130.0
            except ValueError:
                abr = 130.0
            bitrate = int(abr * 1000)
            res = {
                "title": title,
                "videoId": video_id,
                "adaptiveFormats": [
                    {
                        "type": "audio/webm; codecs=\"opus\"",
                        "url": stream_url,
                        "bitrate": bitrate,
                        "itag": "251"
                    }
                ]
            }
            CACHE[cache_key] = (res, now)
            return res
        print(f"[ERROR] yt-dlp video extract failed: {proc.stderr[:100]}")
        return None
    except Exception as e:
        print(f"[ERROR] Video resolution failed: {e}")
        return None
    finally:
        with RESOLVING_LOCK:
            if video_id in RESOLVING_EVENTS:
                RESOLVING_EVENTS[video_id].set()
                del RESOLVING_EVENTS[video_id]

class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True

class InvidiousBridgeHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        # Clean custom logging with timestamps
        sys.stdout.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {self.address_string()} - {format % args}\n")
        sys.stdout.flush()

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        params = urllib.parse.parse_qs(parsed.query)

        # 1. Health check & Stats
        if path in ["/api/v1/stats", "/health", "/stats"]:
            stats = {
                "version": "2.0-bridge",
                "software": {"name": "invidious-pi-bridge", "version": "1.0"},
                "status": "healthy"
            }
            self.send_json(200, stats)
            return

        # 2. Search: /api/v1/search?q=...
        if path == "/api/v1/search":
            q = params.get("q", [""])[0]
            if not q:
                self.send_json(400, {"error": "Missing 'q' query parameter"})
                return
            print(f"\n[INVIDIOUS BRIDGE] -> Search request: '{q}' from {self.client_address[0]}")
            results = search_youtube(q, max_results=5)
            if results:
                print(f"[INVIDIOUS BRIDGE] <- Found {len(results)} tracks (Top: '{results[0]['title']}')")
                # Eagerly pre-fetch stream format for the top track in background!
                top_id = results[0]["videoId"]
                print(f"[INVIDIOUS BRIDGE] -> [EAGER PREFETCH] Starting background resolution for '{top_id}'")
                threading.Thread(target=resolve_video_formats, args=(top_id,), daemon=True).start()
            else:
                print(f"[INVIDIOUS BRIDGE] <- No results for '{q}'")
            self.send_json(200, results)
            return

        # 3. Video Metadata & Audio Formats: /api/v1/videos/<videoId>
        if path.startswith("/api/v1/videos/"):
            video_id = path.split("/api/v1/videos/")[1].split("?")[0].strip()
            if not video_id:
                self.send_json(400, {"error": "Missing videoId"})
                return
            print(f"\n[INVIDIOUS BRIDGE] -> Resolving audio stream for videoId: '{video_id}'")
            details = resolve_video_formats(video_id)
            if not details or not details.get("adaptiveFormats"):
                print(f"[INVIDIOUS BRIDGE] <- Failed to resolve stream for '{video_id}'")
                self.send_json(404, {"error": "Video formats not found"})
                return

            best = details["adaptiveFormats"][0]
            print(f"[INVIDIOUS BRIDGE] <- Extracted stream: {best['type']} @ {best['bitrate']} bps")
            self.send_json(200, details)
            return

        # 4. Root status page
        if path == "/":
            html = f"""<!DOCTYPE html>
<html>
<head><title>Invidious Bridge (Raspberry Pi)</title></head>
<body style="font-family:sans-serif;background:#181818;color:#eee;padding:2rem;">
    <h2>Invidious-Compatible Audio Bridge</h2>
    <p>Status: <span style="color:#4caf50;font-weight:bold;">Running</span></p>
    <p>Endpoints:</p>
    <ul>
        <li><code>GET /api/v1/search?q=song+title</code></li>
        <li><code>GET /api/v1/videos/:videoId</code></li>
        <li><code>GET /api/v1/stats</code></li>
    </ul>
    <p>Target Device: ESP32-S3 Waveshare Native Media Player</p>
</body>
</html>"""
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(html)))
            self.end_headers()
            self.wfile.write(html.encode())
            return

        self.send_json(404, {"error": "Endpoint not found"})

    def send_json(self, status, obj):
        payload = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(payload)

def main():
    parser = argparse.ArgumentParser(description="Invidious-compatible API bridge for Raspberry Pi / local LAN")
    parser.add_argument("--host", default="0.0.0.0", help="Host interface to bind (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=PORT, help=f"HTTP port (default: {PORT})")
    args = parser.parse_args()

    server = ThreadedHTTPServer((args.host, args.port), InvidiousBridgeHandler)
    print("================================================================")
    print(f" Invidious Bridge Server listening on http://{args.host}:{args.port}")
    print(f" Backend extractor: {' '.join(YTDLP_CMD)}")
    print(" Press Ctrl+C to stop.")
    print("================================================================\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server...")
        server.server_close()

if __name__ == "__main__":
    main()
