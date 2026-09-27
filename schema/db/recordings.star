// /sdcard/db/recordings.ndb: the files in /sdcard/recordings. See
// docs/nexus-db-design.md. Tags are never reused; retire a removed field's
// tag with a comment.
//
// Every change is written through: writes are rare (a recording stops, a
// file is renamed or deleted) and each should survive a power cut.
database recordings path="/sdcard/db/recordings.ndb" flush=every_commit {

    // One document per file, keyed by an id as a decimal string ("1", "2",
    // ...) that never changes, so renaming a file rewrites one field. At
    // open the list is checked against the directory: new files are read
    // and added, entries whose file is gone are dropped.
    collection recordings id=1 key=string cache doc=RecordingDoc {
        field file:        string tag=1   // name in /sdcard/recordings
        field started:     u32    tag=2   // epoch s; 0 = unknown (clock not set)
        field duration_ms: u32    tag=3
        field mode:        u8     tag=4   // 0 unknown, 1 stereo (raw mics), 2 processed (AFE output)
        field sample_rate: u32    tag=5   // Hz; Opus: the encoded rate if known, else the input rate
        field channels:    u8     tag=6
        field size:        u32    tag=7   // bytes
    }
}
