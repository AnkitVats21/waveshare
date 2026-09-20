import pytest
import struct
import waveshare_host as wh

def test_wal_sequence_increment_on_mutation(sysdb):
    """Verify that every mutate() call that modifies fields increments WAL sequence."""
    initial_seq = sysdb.wal_head_seq()

    # Mutate speaker volume
    sysdb.mutate(lambda s: setattr(s, "speaker_volume", 65))
    seq1 = sysdb.wal_head_seq()
    assert seq1 > initial_seq, "WAL head sequence should advance on field mutation"

    # Query WAL records since initial_seq
    res, records = sysdb.get_wal_records_since(initial_seq, 10)
    assert res == wh.WalQueryResult.SUCCESS
    assert len(records) >= 1

    rec = records[-1]
    assert rec.seq == seq1
    assert rec.component_id == wh.ComponentId.AUDIO
    assert rec.field_tag == wh.TAG_AUDIO.speaker_volume
    # 65 as 32-bit int
    val = struct.unpack("<i", rec.value)[0]
    assert val == 65

def test_wal_no_increment_when_no_change(sysdb):
    """Verify that mutating with the same value does NOT emit a WAL entry."""
    sysdb.mutate(lambda s: setattr(s, "speaker_volume", 75))
    seq_before = sysdb.wal_head_seq()

    # Mutate with identical value
    sysdb.mutate(lambda s: setattr(s, "speaker_volume", 75))
    seq_after = sysdb.wal_head_seq()
    assert seq_after == seq_before, "WAL head should not advance if field did not change"

def test_write_gate_accepted_writable(sysdb):
    """Verify that process_remote_write accepts writes to FieldAccess::Writable fields."""
    # speaker_volume is int32 (4 bytes)
    payload = struct.pack("<i", 42)
    res = sysdb.process_remote_write(
        wh.ComponentId.AUDIO,
        wh.TAG_AUDIO.speaker_volume,
        payload
    )
    assert res == wh.WriteResult.OK
    assert sysdb.speaker_volume() == 42

def test_write_gate_rejects_readonly(sysdb):
    """Verify that process_remote_write rejects writes to FieldAccess::ReadOnly fields structurally."""
    # wifi_connected is FieldAccess::ReadOnly
    seq_before = sysdb.wal_head_seq()
    res = sysdb.process_remote_write(
        wh.ComponentId.SYSTEM,
        wh.TAG_SYSTEM.wifi_connected,
        b"\x01"
    )
    assert res == wh.WriteResult.REJECTED_READONLY
    assert sysdb.wal_head_seq() == seq_before, "WAL head must not advance on rejected write"
    assert sysdb.wifi_connected() is False

def test_write_gate_rejects_invalid_payload(sysdb):
    """Verify that process_remote_write validates payload length."""
    # speaker_volume expects 4 bytes; provide 2
    res = sysdb.process_remote_write(
        wh.ComponentId.AUDIO,
        wh.TAG_AUDIO.speaker_volume,
        b"\x00\x2a"
    )
    assert res == wh.WriteResult.DECODE_ERROR

def test_write_gate_rejects_unknown_field(sysdb):
    """Verify that process_remote_write rejects unknown component or tag."""
    res = sysdb.process_remote_write(
        wh.ComponentId.AUDIO,
        250, # invalid tag
        b"\x00"
    )
    assert res == wh.WriteResult.INVALID_TAG

def test_export_snapshot(sysdb):
    """Verify export_snapshot exports all state fields with the current head sequence."""
    sysdb.mutate(lambda s: setattr(s, "speaker_volume", 88))
    head_seq, snapshot = sysdb.export_snapshot()

    assert head_seq == sysdb.wal_head_seq()
    assert len(snapshot) > 10, "Snapshot must serialize all component fields"

    # Find the speaker_volume record in snapshot
    found = False
    for r in snapshot:
        assert r.seq == head_seq
        if r.component_id == wh.ComponentId.AUDIO and r.field_tag == wh.TAG_AUDIO.speaker_volume:
            val = struct.unpack("<i", r.value)[0]
            assert val == 88
            found = True
            break
    assert found, "speaker_volume should be in exported snapshot"

def test_catchup_queries(sysdb):
    """Verify catchup query behaviors: UP_TO_DATE, SUCCESS, SNAPSHOT_REQUIRED."""
    head_seq = sysdb.wal_head_seq()

    # Query since current head -> UP_TO_DATE
    res, recs = sysdb.get_wal_records_since(head_seq, 100)
    assert res == wh.WalQueryResult.UP_TO_DATE
    assert len(recs) == 0

    # Query since impossible future sequence -> SNAPSHOT_REQUIRED
    res, recs = sysdb.get_wal_records_since(head_seq + 100, 100)
    assert res == wh.WalQueryResult.SNAPSHOT_REQUIRED
    assert len(recs) == 0
