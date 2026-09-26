// /sdcard/db/system.ndb: device state, settings and alarms. See
// docs/nexus-db-design.md. Tags are never reused; retire a removed field's
// tag with a comment.
database system path="/sdcard/db/system.ndb" flush=every_commit sysdb_include="core_sysdb/SystemState.generated.h" {

    // One document, key "state": the SystemState fields that survive a
    // reboot. sysdb= names the live field; the generated fromSysdb/toSysdb
    // copy them. Defaults must match sysdb.star, since a field missing from
    // the file is applied with its default.
    collection state id=1 key=string cache doc=SavedState {
        field speaker_volume:  i32 = 80    tag=1  sysdb=audio.speaker_volume
        field mic_gain_db:     f32 = 60    tag=2  sysdb=audio.mic_gain_db
        field led_color:       rgb         tag=3  sysdb=led.color
        field led_mode:        u8          tag=4  sysdb=led.mode       // LedMode
        field autoplay:        bool = true tag=5  sysdb=media.autoplay_enabled
        field cache_downloads: bool        tag=6  sysdb=media.cache_downloads
    }
}
