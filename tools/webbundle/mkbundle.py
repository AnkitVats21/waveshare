#!/usr/bin/env python3
"""Pack a built web frontend (e.g. Vite's dist/) into a Nexus web bundle.

The bundle is flashed into one of the www_0/www_1 partitions with
POST /api/ota/frontend and served by Http::WebBundle straight from flash
(components/http_server). Layout, little-endian:

  Header (80 B): magic "NXWB", u16 format=1, u16 file_count, u32 total_size,
                 u32 build_time, char version[32], u8 sha256[32]
  Entries (128 B each): char path[104], u32 offset, u32 size,
                        u32 flags (bit0 = gzip), u8 etag[8], u32 reserved
  File data, each file 4-byte aligned.

sha256 covers the whole bundle with the sha256 field zeroed. Text assets are
stored gzip-compressed (served with Content-Encoding: gzip) when that is
smaller.

Usage:
  mkbundle.py DIST_DIR -o nexus-web.bin [--version TEXT]
  mkbundle.py DIST_DIR -o nexus-web.bin --upload http://192.168.1.14
"""

import argparse
import gzip
import hashlib
import os
import struct
import subprocess
import sys
import time
import urllib.request

MAGIC = b"NXWB"
FORMAT = 1
HEADER = struct.Struct("<4sHHII32s32s")
ENTRY = struct.Struct("<104sIII8sI")
FLAG_GZIP = 1
PATH_MAX = 103          # + NUL
VERSION_MAX = 31        # + NUL
SLOT_CAPACITY = 0xA8000  # www_0/www_1 size in partitions.csv
COMPRESSIBLE = {".html", ".js", ".css", ".json", ".svg", ".txt", ".webmanifest", ".map", ".ico"}

assert HEADER.size == 80 and ENTRY.size == 128


def align4(n):
    return (n + 3) & ~3


def default_version(dist):
    try:
        out = subprocess.run(["git", "describe", "--always", "--dirty"], cwd=dist, capture_output=True, text=True)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    except OSError:
        pass
    return time.strftime("%Y%m%d-%H%M%S")


def collect(dist):
    files = []
    for root, _, names in os.walk(dist):
        for name in names:
            full = os.path.join(root, name)
            rel = "/" + os.path.relpath(full, dist).replace(os.sep, "/")
            if name.endswith(".gz") or name == ".DS_Store":
                continue
            if len(rel.encode()) > PATH_MAX:
                sys.exit(f"path too long for bundle ({PATH_MAX} bytes max): {rel}")
            files.append((rel, full))
    if not any(rel == "/index.html" for rel, _ in files):
        sys.exit(f"{dist} has no index.html")
    return sorted(files)


def build(dist, version, build_time):
    files = collect(dist)
    blobs = []
    for rel, full in files:
        with open(full, "rb") as f:
            raw = f.read()
        data, flags = raw, 0
        if os.path.splitext(rel)[1].lower() in COMPRESSIBLE:
            gz = gzip.compress(raw, compresslevel=9, mtime=0)
            if len(gz) < len(raw):
                data, flags = gz, FLAG_GZIP
        blobs.append((rel, data, flags, len(raw)))

    offset = align4(HEADER.size + ENTRY.size * len(blobs))
    table, body = b"", b""
    for rel, data, flags, _ in blobs:
        etag = hashlib.sha256(data).digest()[:8]
        table += ENTRY.pack(rel.encode(), offset + len(body), len(data), flags, etag, 0)
        body += data + b"\0" * (align4(len(data)) - len(data))
    table += b"\0" * (offset - HEADER.size - len(table))

    total = HEADER.size + len(table) + len(body)
    vbytes = version.encode()[:VERSION_MAX]
    header_zero = HEADER.pack(MAGIC, FORMAT, len(blobs), total, build_time, vbytes, b"\0" * 32)
    sha = hashlib.sha256(header_zero + table + body).digest()
    header = HEADER.pack(MAGIC, FORMAT, len(blobs), total, build_time, vbytes, sha)
    return header + table + body, blobs, sha


def upload(url, bundle):
    req = urllib.request.Request(url.rstrip("/") + "/api/ota/frontend", data=bundle, method="POST",
                                 headers={"Content-Type": "application/octet-stream"})
    with urllib.request.urlopen(req, timeout=120) as resp:
        return resp.read().decode()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dist", help="built frontend directory (contains index.html)")
    ap.add_argument("-o", "--output", default="nexus-web.bin")
    ap.add_argument("--version", help="version label (default: git describe of DIST)")
    ap.add_argument("--upload", metavar="URL", help="also upload to the device, e.g. http://192.168.1.14")
    args = ap.parse_args()

    version = args.version or default_version(args.dist)
    bundle, blobs, sha = build(args.dist, version, int(time.time()))
    if len(bundle) > SLOT_CAPACITY:
        sys.exit(f"bundle is {len(bundle)} bytes; a slot holds {SLOT_CAPACITY}")

    with open(args.output, "wb") as f:
        f.write(bundle)
    for rel, data, flags, raw_len in blobs:
        note = f"gzip {raw_len} -> {len(data)}" if flags & FLAG_GZIP else f"{raw_len}"
        print(f"  {rel:48s} {note}")
    print(f"{args.output}: {len(bundle)} bytes, {len(blobs)} files, version {version!r}, sha256 {sha.hex()[:16]}...")

    if args.upload:
        print("Uploading...", upload(args.upload, bundle))


if __name__ == "__main__":
    main()
