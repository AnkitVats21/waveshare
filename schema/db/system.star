// /sdcard/db/system.ndb: device state, settings, alert chimes and alarms. See
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

    // One document, key "settings". Empty strings mean "use the default":
    // UTC, and the model and voice compiled into the firmware.
    collection settings id=2 key=string cache doc=Settings {
        field timezone:             string = "UTC" tag=1   // POSIX TZ, e.g. "IST-5:30"
        field gemini_model:         string         tag=2
        field gemini_voice:         string         tag=3
        field gemini_system_prompt: string         tag=4
    }

    // One document per alert chime, keyed by its name ("wake_confirm",
    // "ready_to_speak", "session_end", "error", "offline"). No document means
    // the default: the file <name>.ogg in /sdcard/media/alert/ if present,
    // else the built-in tone.
    collection alerts id=3 key=string cache doc=AlertConfig {
        field enabled: bool = true tag=1
        field source:  string      tag=2   // "" default, "builtin", or a file name in /sdcard/media/alert/
        field gain_db: f32         tag=3   // -24..+6
    }
}
