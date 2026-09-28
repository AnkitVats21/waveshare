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
        field transcripts:          bool = true    tag=5   // ask Gemini for transcriptions
        field transcript_log:       bool = true    tag=6   // print each turn to the log
        field manual_silence_s:     u8 = 10        tag=7   // silence timeout of a session started from the API
        field resume_min:           u8 = 60        tag=8   // continue the last conversation if it ended < this many minutes ago; 0 = never
        field keepalive_s:          u8 = 60        tag=9   // keep the connection open this long after a session (mic off); 0 = close at once
        field echo_measure:         bool           tag=10  // log the echo-cancelled mic during replies (barge-in study)
        field barge_in:             bool           tag=11  // stream the mic during replies so speaking interrupts them
        field web_search:           bool = true    tag=12  // give Gemini Google Search (2.5 Live models; 3.x refuses it on the free tier)
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

    // One document per alarm, keyed by its id as a decimal string ("1", "2", ...).
    // When: `at` (epoch seconds) for a one-shot alarm or timer, else hour:minute
    // on the `days` in local time (bit0 Mon .. bit6 Sun; 0 = next occurrence
    // only). A one-shot alarm is disabled after it fires; a timer is deleted.
    collection alarms id=4 key=string cache doc=AlarmDoc {
        field hour:         u8          tag=1
        field minute:       u8          tag=2
        field days:         u8          tag=3
        field at:           u32         tag=4    // epoch s; 0 = use hour/minute
        field label:        string      tag=5
        field enabled:      bool = true tag=6
        field tone:         string      tag=7    // CatalogDB song id; "" = built-in tone
        field snooze_min:   u8 = 9      tag=8
        field volume:       u8          tag=9    // 0 = the alarm floor (AlarmService::MIN_VOLUME)
        field last_fired:   u32         tag=10   // epoch s of the last fire
        field snooze_until: u32         tag=11   // epoch s; 0 = not snoozed
        field kind:         u8          tag=12   // 0 alarm, 1 timer
        field created:      u32         tag=13   // epoch s
    }

    // One document per reminder, keyed like alarms. Same "when" fields.
    collection reminders id=5 key=string cache doc=ReminderDoc {
        field hour:       u8          tag=1
        field minute:     u8          tag=2
        field days:       u8          tag=3
        field at:         u32         tag=4
        field text:       string      tag=5
        field enabled:    bool = true tag=6
        field last_fired: u32         tag=7
        field pending:    bool        tag=8    // fired but not spoken; waits for acknowledgement
        field created:    u32         tag=9
    }
}
