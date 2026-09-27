// /sdcard/db/music.ndb: the music library. See docs/nexus-db-design.md.
// Tags are never reused; retire a removed field's tag with a comment.
//
// Writes are batched (fsync at most every 2 s): a play adds one or two
// records, and losing the last play count in a power cut is harmless.
database music path="/sdcard/db/music.ndb" flush=batched {

    // One document per song ever played or found on the card, keyed by its
    // YouTube video id (e.g. "ApCL2GomTD4"). The saved file is
    // /sdcard/music/<key>.webm (older ones .ogg or .opus, WebM inside); the
    // thumbnail /sdcard/music/thumbs/<key>.jpg. An entry whose file is gone
    // stays, with file_size 0, so its play history is kept.
    collection tracks id=1 key=string cache doc=TrackDoc {
        field title:          string tag=1
        field artist:         string tag=2
        field album:          string tag=3
        field duration_ms:    u32    tag=4   // 0 = unknown
        field file_size:      u32    tag=5   // bytes of the saved file; 0 = not saved
        field added_at:       u32    tag=6   // epoch s the entry was created
        field last_played_at: u32    tag=7   // epoch s
        field play_count:     u32    tag=8
        field thumbnail:      bool   tag=9   // thumbs/<key>.jpg exists
    }
}
