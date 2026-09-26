// Test-only schema for the host tests of the nexus_db generator.
database testdb path="/sdcard/db/testdb.ndb" flush=every_commit {
    collection tracks id=1 key=string cache doc=Track {
        field title:       string          tag=1
        field duration_ms: u32             tag=2
        field play_count:  u32             tag=3
        field volume:      u8 = 80         tag=4
        field gain:        f32 = 1.5       tag=5
        field color:       rgb = 0x10FF20  tag=6
        field pinned:      bool            tag=7
        // tag 8 retired
        field offset:      i64 = -5        tag=9
        field blob:        bytes           tag=10
        field label:       string = "none" tag=11
    }
    collection blobs id=2 key=string doc=Blob {
        field data: bytes tag=1
        field n:    i32   tag=2
    }
}
