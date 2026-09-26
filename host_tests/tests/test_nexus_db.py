"""nexus_db engine: file format, recovery from cut-off writes and corruption,
cleanup interrupted at every step, the generated typed API, and the JS reader.
See docs/nexus-db-design.md."""

import glob
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

import pytest
import waveshare_host as wh

ndb = wh.ndb
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "starc"))
import ndb_gen  # noqa: E402

PATH = "/db/test.ndb"
CACHED, RAW = 1, 2
COLLECTIONS = [(CACHED, "docs", True), (RAW, "raw", False)]
HEADER = 32


# --- helpers ----------------------------------------------------------------

def field(tag, data: bytes) -> bytes:
    return struct.pack("<HH", tag, len(data)) + data


def u32(tag, v):
    return field(tag, struct.pack("<I", v))


def fields_of(value: bytes) -> dict:
    out, p = {}, 0
    while len(value) - p >= 4:
        tag, n = struct.unpack_from("<HH", value, p)
        out[tag] = value[p + 4:p + 4 + n]
        p += 4 + n
    return out


def merged(old: bytes, patch: bytes) -> bytes:
    """What the device's merge produces (fields replaced by tag)."""
    ptags = fields_of(patch).keys()
    keep, p = b"", 0
    while len(old) - p >= 4:
        tag, n = struct.unpack_from("<HH", old, p)
        if tag not in ptags:
            keep += old[p:p + 4 + n]
        p += 4 + n
    return keep + patch


class Env:
    def __init__(self, tmp_path):
        self.root = str(tmp_path)
        self.file = Path(self.root + PATH)
        self.tmp = Path(self.root + PATH + ".tmp")

    def io(self, **budgets):
        io = ndb.FaultIo()
        io.root = self.root
        for k, v in budgets.items():
            setattr(io, k, v)
        return io

    def db(self, io=None, every_commit=True, compact_min_bytes=64 * 1024):
        io = io or self.io()
        return ndb.Database(PATH, "test", COLLECTIONS, every_commit=every_commit,
                            compact_min_bytes=compact_min_bytes, io=io)

    def opened(self, **kw):
        d = self.db(**kw)
        assert d.open()
        return d

    def state(self, d=None):
        d = d or self.opened()
        return {CACHED: dict(d.items(CACHED)), RAW: dict(d.items(RAW))}


@pytest.fixture
def env(tmp_path):
    return Env(tmp_path)


def records(data: bytes):
    """(offset, length, seq, collection, op, key, value) of each record."""
    out, pos = [], HEADER
    while pos + 16 <= len(data):
        sync, length, crc = struct.unpack_from("<HHI", data, pos)
        assert sync == 0x5AA5
        assert zlib.crc32(data[pos + 8:pos + length]) == crc
        seq, coll, op, klen, _ = struct.unpack_from("<IBBBB", data, pos + 8)
        key = data[pos + 16:pos + 16 + klen].decode()
        out.append((pos, length, seq, coll, op, key, data[pos + 16 + klen:pos + length]))
        pos += length
    assert pos == len(data)
    return out


# --- basics -----------------------------------------------------------------

def test_put_get_merge_delete_survive_reopen(env):
    d = env.opened()
    assert d.put(CACHED, "a", u32(1, 7) + field(2, b"hello"))
    assert d.merge(CACHED, "a", u32(1, 8) + u32(3, 1))
    assert d.merge(CACHED, "new", u32(5, 5))  # merge creates the document
    assert d.put(RAW, "r1", field(1, b"x" * 300))
    assert d.put(RAW, "r2", field(1, b"y"))
    assert d.remove(RAW, "r2")
    assert not d.merge(RAW, "r1", u32(1, 1)), "merge is for cached collections only"
    before = env.state(d)
    assert fields_of(before[CACHED]["a"]) == {1: struct.pack("<I", 8), 2: b"hello", 3: struct.pack("<I", 1)}
    assert set(before[RAW]) == {"r1"}
    d.close()
    d2 = env.opened()
    assert env.state(d2) == before
    assert d2.get(RAW, "r1") == field(1, b"x" * 300)
    assert d2.get(RAW, "r2") is None
    assert d2.count(CACHED) == 2


def test_unchanged_writes_append_nothing(env):
    d = env.opened()
    d.put(CACHED, "a", u32(1, 7))
    size = env.file.stat().st_size
    assert d.put(CACHED, "a", u32(1, 7))
    assert d.merge(CACHED, "a", u32(1, 7))
    assert d.remove(CACHED, "missing")
    assert env.file.stat().st_size == size


def test_rejects_bad_keys_and_collections(env):
    d = env.opened()
    assert not d.put(CACHED, "k" * 65, b"")
    assert d.put(CACHED, "k" * 64, b"")
    assert not d.put(9, "a", b"")
    assert not d.put(CACHED, "big", field(1, b"x" * 60000) + field(2, b"x" * 6000))


def test_file_layout_matches_design(env):
    d = env.opened()
    d.put(CACHED, "abc", u32(1, 5))
    d.merge(CACHED, "abc", u32(2, 6))
    d.remove(CACHED, "abc")
    d.close()
    data = env.file.read_bytes()
    magic, ver, hlen, shash, gen, name, crc = struct.unpack_from("<4sHHII12sI", data)
    assert (magic, ver, hlen, shash, gen) == (b"NXDB", 1, 32, 0x1234, 0)
    assert name.rstrip(b"\0") == b"test"
    assert crc == zlib.crc32(data[:28])
    recs = records(data)
    assert [(r[2], r[3], r[4], r[5]) for r in recs] == [(1, CACHED, 1, "abc"), (2, CACHED, 2, "abc"), (3, CACHED, 3, "abc")]
    assert all(r[1] % 4 == 0 for r in recs)
    assert recs[0][6] == u32(1, 5) + b"\0"  # 16 + 3 + 8 bytes, padded to 28


def test_batched_mode_defers_fsync(env):
    io = env.io()
    d = env.db(io=io, every_commit=False)
    assert d.open()
    base = io.ops
    for i in range(20):
        d.put(CACHED, f"k{i}", u32(1, i))
    assert io.ops - base == 20, "one append per put, no fsync inside the 2 s window"
    d.flush()
    assert io.ops - base == 21


# --- recovery at open ---------------------------------------------------------

def fill(d, n=5):
    for i in range(n):
        assert d.put(CACHED, f"k{i}", u32(1, i) + field(2, b"v" * (i * 7)))


def test_cut_off_tail_is_truncated_at_every_length(env):
    d = env.opened()
    fill(d)
    d.close()
    data = env.file.read_bytes()
    last = records(data)[-1]
    for cut in range(last[0], len(data)):
        env.file.write_bytes(data[:cut])
        d = env.opened()
        s = d.stats()
        assert s["documents"] == 4 and s["truncated_bytes"] == cut - last[0]
        d.close()
        assert env.file.stat().st_size == last[0]


def test_trailing_zeros_are_truncated(env):
    d = env.opened()
    fill(d)
    d.close()
    size = env.file.stat().st_size
    with open(env.file, "ab") as f:
        f.write(b"\0" * 4096)
    d = env.opened()
    assert d.count(CACHED) == 5
    d.close()
    assert env.file.stat().st_size == size


def test_damaged_record_mid_file_is_skipped_and_cleaned_up(env):
    d = env.opened()
    fill(d)
    expected = env.state(d)
    d.close()
    data = bytearray(env.file.read_bytes())
    victim = records(bytes(data))[2]
    data[victim[0] + 20] ^= 0xFF
    env.file.write_bytes(bytes(data))
    d = env.opened()
    s = d.stats()
    del expected[CACHED][victim[5]]
    assert env.state(d) == expected
    assert s["skipped_bytes"] == victim[1]
    assert s["generation"] == 1 and s["compactions"] == 1, "damage schedules a cleanup"
    d.close()
    d = env.opened()
    assert d.stats()["skipped_bytes"] == 0
    assert env.state(d) == expected


def test_bad_header_sets_file_aside(env):
    d = env.opened()
    fill(d)
    d.close()
    data = bytearray(env.file.read_bytes())
    data[5] ^= 0x01
    env.file.write_bytes(bytes(data))
    d = env.opened()
    assert d.count(CACHED) == 0
    assert Path(str(env.file) + ".bad").read_bytes() == bytes(data)


def test_newer_format_is_left_alone(env):
    d = env.opened()
    fill(d)
    d.close()
    data = bytearray(env.file.read_bytes())
    struct.pack_into("<H", data, 4, 2)
    struct.pack_into("<I", data, 28, zlib.crc32(bytes(data[:28])))
    env.file.write_bytes(bytes(data))
    d = env.db()
    assert not d.open()
    assert env.file.read_bytes() == bytes(data)


def test_schema_change_rewrites_header_once(env):
    d = env.opened()
    fill(d)
    expected = env.state(d)
    d.close()
    data = bytearray(env.file.read_bytes())
    struct.pack_into("<I", data, 8, 0x9999)  # written by another schema
    struct.pack_into("<I", data, 28, zlib.crc32(bytes(data[:28])))
    env.file.write_bytes(bytes(data))
    d = env.opened()
    assert d.stats()["compactions"] == 1
    assert env.state(d) == expected
    d.close()
    assert struct.unpack_from("<I", env.file.read_bytes(), 8)[0] == 0x1234
    d = env.opened()
    assert d.stats()["compactions"] == 0


def test_unknown_collection_records_are_ignored(env):
    other = ndb.Database(PATH, "test", COLLECTIONS + [(7, "future", True)], io=env.io())
    assert other.open()
    other.put(7, "x", u32(1, 1))
    other.put(CACHED, "a", u32(1, 2))
    other.close()
    d = env.opened()
    assert set(env.state(d)[CACHED]) == {"a"}


# --- cleanup ------------------------------------------------------------------

def test_cleanup_runs_when_mostly_dead(env):
    d = env.opened(compact_min_bytes=4096)
    for i in range(400):
        d.put(CACHED, f"k{i % 10}", u32(1, i) + field(2, b"x" * 40))
        d.put(RAW, f"r{i % 3}", u32(1, i))
    s = d.stats()
    assert s["compactions"] >= 1
    assert s["file_bytes"] < 4096 * 2
    expected = env.state(d)
    d.close()
    recs = records(env.file.read_bytes())
    assert 4 in [r[4] for r in recs], "a cleanup ends with a seal record"
    d = env.opened()
    assert env.state(d) == expected
    assert d.stats()["generation"] == s["generation"]


def build_dirty(env):
    d = env.opened()
    for i in range(60):
        d.put(CACHED, f"k{i % 7}", u32(1, i) + field(2, b"x" * (i % 13)))
        if i % 3 == 0:
            d.merge(CACHED, f"k{i % 7}", u32(3, i))
        d.put(RAW, f"r{i % 4}", field(1, bytes([i]) * 20))
        if i % 11 == 0:
            d.remove(CACHED, f"k{(i + 3) % 7}")
    expected = env.state(d)
    d.close()
    return expected


@pytest.mark.parametrize("budget_kind", ["op_budget", "write_budget"])
def test_cleanup_interrupted_at_every_step(env, tmp_path, budget_kind):
    expected = build_dirty(env)
    pristine = env.file.read_bytes()
    size = len(pristine)
    budget, completed = 0, False
    while not completed:
        env.file.write_bytes(pristine)
        if env.tmp.exists():
            env.tmp.unlink()
        io = env.io()
        d = env.db(io=io)
        assert d.open()
        setattr(io, budget_kind, budget)
        completed = d.compact() and not io.dead
        d = None  # power lost: nothing else runs
        d = env.opened()
        assert env.state(d) == expected, f"{budget_kind}={budget}"
        d.close()
        assert not env.tmp.exists()
        budget += 1 if budget_kind == "op_budget" else 97
        assert budget < 100000
    assert env.file.stat().st_size < size


def test_cleanup_postponed_while_file_busy(env):
    expected = build_dirty(env)
    io = env.io()
    d = env.db(io=io)
    assert d.open()
    io.busy = True  # a dashboard download holds the file open
    size = env.file.stat().st_size
    assert not d.compact()
    assert not env.tmp.exists()
    assert env.state(d) == expected
    assert d.put(CACHED, "after", u32(1, 1)), "the database keeps working"
    io.busy = False
    assert d.compact()
    assert env.file.stat().st_size < size


def sealed_copy(env):
    """A finished cleanup copy (.tmp) and the state it holds."""
    expected = build_dirty(env)
    d = env.opened()
    assert d.compact()
    d.close()
    return expected, env.file.read_bytes()


def test_sealed_tmp_without_original_is_promoted(env):
    expected, copy = sealed_copy(env)
    env.file.unlink()
    env.tmp.write_bytes(copy)
    assert env.state() == expected
    assert not env.tmp.exists() and env.file.exists()


def test_unsealed_tmp_is_never_used(env):
    expected, copy = sealed_copy(env)
    seal = records(copy)[-1]
    assert seal[4] == 4
    # Without its seal (cut off), and with a seal that doesn't match the count.
    bad_seal = bytearray(copy)
    struct.pack_into("<I", bad_seal, seal[0] + 16 + 4, 999)
    struct.pack_into("<I", bad_seal, seal[0] + 4, zlib.crc32(bytes(bad_seal[seal[0] + 8:seal[0] + seal[1]])))
    for broken in (copy[:seal[0]], copy[:seal[0] + 5], bytes(bad_seal)):
        env.file.unlink(missing_ok=True)
        env.tmp.write_bytes(broken)
        assert env.state() == {CACHED: {}, RAW: {}}
        assert not env.tmp.exists()


# --- power loss during a random workload ----------------------------------------

def run_workload(env, io, seed, steps=150):
    """Returns (acked state, in-flight op or None)."""
    rnd = random.Random(seed)
    state = {CACHED: {}, RAW: {}}
    d = env.db(io=io, compact_min_bytes=2048)
    if not d.open():
        return state, None
    for _ in range(steps):
        coll = rnd.choice([CACHED, RAW])
        key = f"k{rnd.randrange(8)}"
        kind = rnd.choice(["put", "put", "merge", "remove"]) if coll == CACHED else rnd.choice(["put", "put", "remove"])
        value = u32(rnd.randrange(1, 4), rnd.randrange(1 << 30)) + field(4, os.urandom(rnd.randrange(40)))
        if kind == "put":
            ok = d.put(coll, key, value)
        elif kind == "merge":
            ok = d.merge(coll, key, value)
        else:
            ok = d.remove(coll, key)
        op = (coll, kind, key, value)
        if not ok:
            assert io.dead
            return state, op
        apply(state, op)
    return state, None


def apply(state, op):
    coll, kind, key, value = op
    if kind == "put":
        state[coll][key] = value
    elif kind == "merge":
        state[coll][key] = merged(state[coll].get(key, b""), value)
    else:
        state[coll].pop(key, None)


@pytest.mark.parametrize("budget_kind", ["write_budget", "op_budget"])
def test_power_loss_never_loses_acknowledged_writes(env, budget_kind):
    # A full run to size the budgets.
    io = env.io()
    run_workload(env, io, seed=1)
    total = io.bytes if budget_kind == "write_budget" else io.ops
    outcomes = {"before": 0, "after": 0}
    for budget in range(0, total, max(1, total // 400)):
        shutil.rmtree(env.root + "/db", ignore_errors=True)
        acked, inflight = run_workload(env, env.io(**{budget_kind: budget}), seed=1)
        got = env.state()
        if inflight is None or got == acked:
            assert got == acked, f"{budget_kind} {budget}"
            outcomes["before"] += 1
        else:
            after = {CACHED: dict(acked[CACHED]), RAW: dict(acked[RAW])}
            apply(after, inflight)
            assert got == after, f"{budget_kind} {budget}: neither before nor after the in-flight write"
            outcomes["after"] += 1
    # Both outcomes occur: a write lost with the power, and one that reached
    # the card although its fsync never returned.
    if budget_kind == "op_budget":
        assert outcomes["before"] and outcomes["after"], outcomes


# --- generated typed API ---------------------------------------------------------

def test_generated_api(env):
    t = ndb.TestdbDb(env.io())
    assert t.open()
    default = t.default_track()
    assert default["volume"] == 80 and default["label"] == "none" and default["offset"] == -5
    assert default["color"] == (0x10, 0xFF, 0x20) and abs(default["gain"] - 1.5) < 1e-6
    assert t.put_track("ApCL2GomTD4", "Song", 180000, 3)
    assert t.merge_plays("ApCL2GomTD4", 4, True)
    got = t.get_track("ApCL2GomTD4")
    assert (got["title"], got["duration_ms"], got["play_count"], got["pinned"]) == ("Song", 180000, 4, True)
    assert got["blob"] == b"\x00\x01\xff" and got["volume"] == 80
    assert t.put_blob("b", b"\x00" * 1000, -7)
    assert t.get_blob("b") == (b"\x00" * 1000, -7)
    t.close()
    t = ndb.TestdbDb(env.io())
    assert t.open()
    assert t.get_track("ApCL2GomTD4") == got
    assert t.track_keys() == ["ApCL2GomTD4"]
    assert t.remove_track("ApCL2GomTD4") and t.get_track("ApCL2GomTD4") is None


def test_generated_decode_skips_unknown_and_mistyped_fields(env):
    raw = ndb.Database("/sdcard/db/testdb.ndb", "testdb", [(1, "tracks", True), (2, "blobs", False)], io=env.io())
    assert raw.open()
    # title ok, duration as 2 bytes (wrong size), unknown tag 999, retired tag 8
    raw.put(1, "x", field(1, b"T") + field(2, b"\x01\x02") + field(999, b"zz") + u32(8, 1))
    raw.close()
    t = ndb.TestdbDb(env.io())
    assert t.open()
    got = t.get_track("x")
    assert got["title"] == "T" and got["duration_ms"] == 0 and got["volume"] == 80


def test_schema_hash_matches_generator():
    s = ndb_gen.parse((ROOT / "host_tests/schema/testdb.star").read_text(), "testdb.star")
    assert ndb.TestdbDb.schema_hash == ndb_gen.schema_hash(s)


@pytest.mark.parametrize("bad, msg", [
    ("database x path=\"/a\" {\n collection c id=1 {\n field a: u8 tag=1\n field b: u8 tag=1\n }\n}", "used twice"),
    ("database x path=\"/a\" {\n collection c id=1 {\n field a: float tag=1\n }\n}", "unknown type"),
    ("database x path=\"/a\" {\n collection c id=0 {\n }\n}", "1..255"),
    ("database x {\n}", "path="),
    ("database x path=\"/a\" {\n collection c id=1 {\n field a: u8 tag=1 sysdb=audio.volume\n }\n}", "sysdb_include"),
    ("database x path=\"/a\" sysdb_include=\"h.h\" {\n collection c id=1 {\n field a: u8 tag=1 sysdb=volume\n }\n}",
     "component.field"),
    ("database x path=\"/a\" {\n collection c id=1 {\n field a: u8 tag=1 persist=1\n }\n}", "unknown field option"),
])
def test_generator_rejects_bad_schemas(bad, msg):
    with pytest.raises(ndb_gen.SchemaError, match=msg):
        ndb_gen.parse(bad, "bad.star")


def test_system_schema_bindings():
    s = ndb_gen.parse((ROOT / "schema/db/system.star").read_text(), "system.star")
    state = next(c for c in s.collections if c.name == "state")
    assert state.cached and state.bound
    assert {f.name: f.sysdb for f in state.fields}["speaker_volume"] == "audio.speaker_volume"
    src = ndb_gen.gen_source(s, "SystemDb.generated.h")
    assert "nexus_db::assignField(s.audio.speaker_volume, speaker_volume);" in src
    assert '#include "core_sysdb/SystemState.generated.h"' in src


# --- JS reader ---------------------------------------------------------------------

def find_node():
    node = shutil.which("node")
    if node:
        return node
    found = sorted(glob.glob(os.path.expanduser("~/.nvm/versions/node/*/bin/node")))
    return found[-1] if found else None


def test_js_reader_matches_device(env):
    node = find_node()
    if not node:
        pytest.skip("node not installed")
    t = ndb.TestdbDb(env.io())
    assert t.open()
    for i in range(30):
        t.put_track(f"id{i:02d}", f"Title {i} ü", 1000 * i, i)
    for i in range(0, 30, 3):
        t.merge_plays(f"id{i:02d}", 100 + i, i % 2 == 0)
    for i in range(0, 30, 7):
        t.remove_track(f"id{i:02d}")
    t.put_blob("b1", b"\x01\x02", 5)
    expected = {k: t.get_track(k) for k in t.track_keys()}
    t.close()
    dbfile = env.root + "/sdcard/db/testdb.ndb"
    # A record cut off at the end, as when a download races an append.
    with open(dbfile, "ab") as f:
        f.write(b"\xa5\x5a\x40\x00" + b"\x11" * 20)
    schema_js = ROOT / "host_tests/build/generated/ndb_schema.js"
    out = subprocess.run([node, str(ROOT / "tools/starc/js/ndb_dump.mjs"), dbfile, str(schema_js)],
                         capture_output=True, text=True, check=True)
    js = json.loads(out.stdout)
    assert js["schemaMatches"] and js["header"]["name"] == "testdb"
    assert js["stats"]["truncated"] == 24
    tracks = js["collections"]["tracks"]
    assert sorted(tracks) == sorted(expected)
    for k, dev in expected.items():
        doc = tracks[k]
        for name in ("title", "duration_ms", "play_count", "volume", "pinned", "offset", "label"):
            assert doc[name] == dev[name], (k, name)
        assert abs(doc["gain"] - dev["gain"]) < 1e-6
        assert doc["color"] == (dev["color"][0] << 16) | (dev["color"][1] << 8) | dev["color"][2]
        assert bytes.fromhex(doc["blob"]) == dev["blob"]
    assert js["collections"]["blobs"]["b1"]["n"] == 5
