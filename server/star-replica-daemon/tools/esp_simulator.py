#!/usr/bin/env python3
"""
STAR Replica Daemon — ESP32 client simulator.

Impersonates the Waveshare ESP32-S3's StarWsClient over the wire so the real
star-replica-daemon binary can be load/stall tested without flashing hardware.
Speaks the exact StarProtocol framing from
components/core_sysdb/include/core_sysdb/StarProtocol.h.

Usage:
    python3 esp_simulator.py [--host 127.0.0.1] [--port 8765] [--rate-hz 2]
                              [--duration 0] [--dashboard-storm N]

Run the real daemon in one terminal:
    ./build/star-replica-daemon 8765
Then run this in another terminal and watch both sides' logs.

What it does every send cycle:
    - Builds a WAL_BATCH frame with one fake AUDIO.speaker_volume mutation
      (same shape as what SpeakerPlayback/AudioService pushes on the real board).
    - Times how long the send call takes (mirrors the ESP's
      esp_websocket_client_send_bin(..., timeout=2000ms) call in
      main/services/network/StarWsClient.cpp:137).
    - Flags any send that took >200ms (the library's internal keepalive lock
      timeout that was firing in the field) as a stall.

Optionally opens N extra "dashboard" websocket connections
(--dashboard-storm) that spam volume-change commands concurrently, to test
whether a busy dashboard can starve the primary ESP<->daemon link on the
daemon's single-threaded ASIO event loop.
"""
import argparse
import asyncio
import struct
import time

import websockets

# StarProtocol.h
MSG_WAL_BATCH = 0x01
MSG_REQ_CATCHUP = 0x02
MSG_SNAPSHOT_START = 0x03
MSG_SNAPSHOT_FIELD = 0x04
MSG_SNAPSHOT_END = 0x05
MSG_CMD_SET_FIELD = 0x06
MSG_CMD_EXEC_ACTION = 0x07
MSG_CMD_ACK = 0x08

HEADER_SIZE = 3  # msg_type:u8 + length:u16 BE

COMP_AUDIO = 1
TAG_AUDIO_SPEAKER_VOLUME = 0  # matches TAG_AUDIO::speaker_volume ordinal used elsewhere


def build_wal_batch_frame(seq: int, component_id: int, field_tag: int, value: bytes) -> bytes:
    # WalWireRecord: seq:u32 LE, component_id:u8, field_tag:u8, val_len:u8, value[]
    record = struct.pack("<IBBB", seq, component_id, field_tag, len(value)) + value
    payload = record
    header = struct.pack(">BH", MSG_WAL_BATCH, len(payload))
    return header + payload


def build_catchup_req_frame(since_seq: int) -> bytes:
    payload = struct.pack("<I", since_seq)
    header = struct.pack(">BH", MSG_REQ_CATCHUP, len(payload))
    return header + payload


def parse_frame_header(data: bytes):
    msg_type, length = struct.unpack(">BH", data[:HEADER_SIZE])
    return msg_type, length, data[HEADER_SIZE:HEADER_SIZE + length]


class Stats:
    def __init__(self):
        self.sent = 0
        self.stalls = 0
        self.max_latency_ms = 0.0
        self.errors = 0

    def report(self):
        print(f"\n=== ESP simulator stats ===")
        print(f"  frames sent   : {self.sent}")
        print(f"  send errors   : {self.errors}")
        print(f"  stalls(>200ms): {self.stalls}")
        print(f"  max latency   : {self.max_latency_ms:.1f} ms")


async def esp_client(uri: str, rate_hz: float, duration: float, stats: Stats):
    print(f"[esp-sim] connecting to {uri} ...")
    async with websockets.connect(uri, max_size=None) as ws:
        print("[esp-sim] connected. Waiting for REQ_CATCHUP from daemon...")

        async def reader():
            async for raw in ws:
                if isinstance(raw, str):
                    continue
                msg_type, length, payload = parse_frame_header(raw)
                if msg_type == MSG_REQ_CATCHUP:
                    since_seq = struct.unpack("<I", payload[:4])[0]
                    print(f"[esp-sim] <- REQ_CATCHUP since_seq={since_seq}")
                elif msg_type == MSG_CMD_SET_FIELD:
                    comp, tag, vlen = payload[0], payload[1], payload[2]
                    print(f"[esp-sim] <- CMD_SET_FIELD comp={comp} tag={tag} len={vlen}")
                elif msg_type == MSG_CMD_EXEC_ACTION:
                    cmd_id = payload[0]
                    print(f"[esp-sim] <- CMD_EXEC_ACTION cmd_id={cmd_id}")
                else:
                    print(f"[esp-sim] <- msg_type=0x{msg_type:02x} ({length}B)")

        reader_task = asyncio.create_task(reader())

        seq = 1
        start = time.monotonic()
        period = 1.0 / rate_hz if rate_hz > 0 else 0.5
        try:
            while duration <= 0 or (time.monotonic() - start) < duration:
                vol = 40 + (seq % 60)
                frame = build_wal_batch_frame(seq, COMP_AUDIO, TAG_AUDIO_SPEAKER_VOLUME,
                                               struct.pack("<i", vol))
                t0 = time.monotonic()
                try:
                    await asyncio.wait_for(ws.send(frame), timeout=2.0)
                except asyncio.TimeoutError:
                    stats.errors += 1
                    print(f"[esp-sim] !! send() TIMED OUT after 2000ms (seq={seq})")
                    continue
                except Exception as e:
                    stats.errors += 1
                    print(f"[esp-sim] !! send() failed: {e!r}")
                    continue
                dt_ms = (time.monotonic() - t0) * 1000.0
                stats.sent += 1
                stats.max_latency_ms = max(stats.max_latency_ms, dt_ms)
                if dt_ms > 200:
                    stats.stalls += 1
                    print(f"[esp-sim] !! stall: send took {dt_ms:.1f}ms (seq={seq}) "
                          f"— would have tripped 'Could not lock ws-client within 200 timeout' on real firmware")
                seq += 1
                await asyncio.sleep(period)
        finally:
            reader_task.cancel()


async def dashboard_storm(uri: str, n: int, duration: float):
    """Open N dashboard-style connections spamming volume commands, to test
    whether a busy dashboard can stall the primary ESP link (single ASIO thread)."""
    async def one(idx: int):
        try:
            async with websockets.connect(uri) as ws:
                start = time.monotonic()
                i = 0
                while duration <= 0 or (time.monotonic() - start) < duration:
                    await ws.send('{"cmd":"volume","value":%d}' % (i % 100))
                    i += 1
                    await asyncio.sleep(0.01)
        except Exception as e:
            print(f"[dash-{idx}] error: {e!r}")

    await asyncio.gather(*(one(i) for i in range(n)))


async def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--rate-hz", type=float, default=2.0, help="WAL_BATCH send rate (Hz)")
    ap.add_argument("--duration", type=float, default=0, help="seconds to run, 0 = forever")
    ap.add_argument("--dashboard-storm", type=int, default=0,
                     help="also open N concurrent dashboard clients spamming volume cmds")
    args = ap.parse_args()

    esp_uri = f"ws://{args.host}:{args.port}/api/star/ws"
    dash_uri = f"ws://{args.host}:{args.port}/dashboard"

    stats = Stats()
    tasks = [esp_client(esp_uri, args.rate_hz, args.duration, stats)]
    if args.dashboard_storm > 0:
        print(f"[esp-sim] also launching {args.dashboard_storm} dashboard-storm clients")
        tasks.append(dashboard_storm(dash_uri, args.dashboard_storm, args.duration))

    try:
        await asyncio.gather(*tasks)
    except KeyboardInterrupt:
        pass
    finally:
        stats.report()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
