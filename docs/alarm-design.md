# Alarms and reminders

Alarms that actually ring. When an alarm fires, everything else stops: the assistant
session ends, chimes go quiet and music is put aside. The device plays only the
alarm until someone stops or snoozes it, then puts the music back.

Reminders share the scheduler: same "when", different delivery. A reminder does not
take over the device; it chimes and speaks its text over whatever is going on (see
*Reminders*).

Status: design. Step A (ringing) is next; B–D follow the same pattern as the alert
refactor (storage, routes, dashboard) and cover alarms and reminders together.

## Where we start

- `AlarmService` loads `/sdcard/alarms.json` (`id`, `hour`, `minute`, `tone_file`,
  `enabled`). `begin()` only loads it: the ringing task is never started
  (`main.cpp`), so no alarm has ever rung.
- `triggerAlarm()` sets `alarm.playing` in sysdb and calls
  `AudioOrchestrator::notifyAlarmStarted()`, but nothing plays: the tone path is a
  commented-out call to a player that no longer exists.
- The scheduler polls every 5 s and matches `hour:minute`, remembering the last
  minute in a function-level `static`.
- Tones are WAV paths (`/sdcard/alarms/*.wav`). No WAV decoder is wired up, and
  none will be: see below.
- Key 2 sets `alarm.stop_requested`; `/api/alarms/stop` and the Gemini
  `stop_active_alarm` tool call `stopActiveAlarm()`.
- `notifyAlarmStarted()` sends `LOSS_PAUSE` to the media track;
  `notifyAlarmEnded()` sends `GAIN`. NexusPlayer's pause keeps its pipeline, which
  the alarm itself needs, so this pair cannot be used as-is.

## Decisions

1. **No WAV tones.** Music is stopped while the alarm rings, so the one decode
   pipeline (NexusPlayer) is free. An alarm tone is a **library song** (a track in
   `CatalogDB`, Ogg or WebM Opus on the SD card) or the **built-in alarm tone**.
2. **The alarm owns NexusPlayer while it rings.** It is not a second player: the
   alarm takes the one pipeline, remembers what was playing, and gives it back.
3. **The built-in tone never touches the SD card.** It is synthesised into PSRAM at
   boot and played from the alert mixer (`AlertMixer`), so a missing file, a bad
   card or a failed decode still produces sound.
4. **An alarm plays a program: a list of items.** Step A has one item (a song or
   the built-in tone, looped). The list shape is there so a briefing alarm (news
   fetched from a server, then music; see *Later: briefing alarms*) is a new item
   kind, not a redesign.
5. **While ringing, the alarm wins every conflict.** Wake word suppressed, chimes
   muted, keys captured, music requests stop the alarm first.

## Ringing

### State machine

```
           fire / test                   snooze
  IDLE ─────────────────▶ RINGING ─────────────────▶ SNOOZED
   ▲                        │  │                       │
   │   stop / timeout       │  │ item fails            │ snooze ends
   └────────────────────────┘  ▼                       │
                          (fall back to              ◀─┘ back to RINGING
                           built-in tone)
```

- **RINGING** plays the program. A song that ends is restarted, so a short song
  still rings for the whole time.
- **Timeout:** after `RING_LIMIT` (10 min) of ringing the alarm stops itself and is
  logged as missed. Timeout is not a snooze.
- **SNOOZED** is silent for `SNOOZE` (9 min), then rings again. Music stays put
  aside during a snooze; it comes back only on the final stop.
- **Stop** (key, API, voice tool, timeout) ends the alarm and restores the music.

The state machine is portable C++ (`AlarmRing`, no ESP-IDF includes): it takes
events (`fire`, `stop`, `snooze`, `tick(now)`, `itemFailed`, `itemEnded`) and
returns actions (`playItem`, `playBuiltin`, `silence`, `restoreMedia`). The clock
is passed in, so ring limits, snooze and fallbacks are host-tested without
waiting minutes. `AlarmService` is the ESP-IDF side that runs it and carries out
the actions.

### Taking over the device

In order, when an alarm starts:

| What | How |
|---|---|
| Assistant session | Set `assistant.session_state = Idle` (the same transition the VAD timeout makes) and flush the voice track. Wake word suppressed (`WakeWordEngine::setWakeWordSuppressed(true)`) until the alarm ends. |
| Chimes | `AlertPlayer::stop()`. `playAlert()` is ignored while an alarm rings (previews too), so the "session end" chime from the line above never plays over the alarm. |
| Music | `NexusPlayer::beginAlarm()` saves the song id, stream URL, position and whether it was playing, then stops the pipelines (not a pause: the alarm reuses them). |
| Keys | Captured: Key 2 stops, any other key snoozes. Volume and play/pause keys do nothing else while ringing. |
| Volume | Speaker volume raised to at least `ALARM_MIN_VOLUME` (60) and restored afterwards; the alarm fades in from 10 % to full over 10 s. |
| Recording | Left running. It is user-started and may be the reason the device is awake. |

### NexusPlayer: the alarm as owner

NexusPlayer gets an owner flag, `MUSIC` (normal) or `ALARM`.

- `beginAlarm()` saves the resume point (`songId`, download URL, position,
  was-playing), stops the active pipelines and sets the owner to `ALARM`.
- `playAlarm(songId)` plays a local track through the normal local path, bypassing
  the assistant-session deferral (`_session_active`), which does not apply.
- While the owner is `ALARM`:
  - `IPlaybackObserver` events are **not** sent to `MusicPlaybackService`, so the
    queue does not advance when the alarm song ends and a missing alarm file is not
    "fixed" by deleting it and streaming instead. `AlarmService` gets its own
    finished/error callback instead.
  - Focus events from the orchestrator are ignored.
  - A music command (`play`, `resume`, next, …) from the dashboard or voice is
    rejected with a log line; the caller stops the alarm first (the routes do
    this: whoever sends a play command is awake).
- `endAlarm(restore)` stops the alarm song, sets the owner back to `MUSIC` and, if
  music was playing, `playAt(songId, url, position)`. A local file seeks through
  the catalog's seek table; a stream reopens its URL at the byte offset. A stream
  URL that has expired (googlevideo URLs last hours, alarms usually ring the next
  morning) fails like any stream error and `MusicPlaybackService` re-resolves it.
  If music was paused, the track stays selected but stopped.

### Orchestrator

`AudioOrchestrator` gains the alarm as the top priority rather than a pause/resume
pair:

- `notifyAlarmStarted()` records `m_alarm_active`; it no longer broadcasts
  `LOSS_PAUSE` (NexusPlayer is already handed over by `beginAlarm()`).
- `notifyMediaStarted()` while an alarm rings must not reset the media gain to 1.0:
  the fade-in uses the media gain (`SpeakerPlaybackTask::setMediaGain(target,
  rampMs)`), and the alarm song starts through the media path.
- `notifyAlertStarted()` is ignored while an alarm rings (no ducking of the alarm).
- `notifyAlarmEnded()` unducks and lets NexusPlayer's restore run.

### Fallback to the built-in tone

The built-in tone is an extra `AlertMixer` slot (`ALERT_ALARM`), synthesised at
boot with `synthesizeTones` (about 1.5 s of beeps and silence). The mixer gains a
`loop` flag per `play()`, so it repeats without the alarm task re-triggering it.

The alarm switches to it when:

- the tone is not set, not in `CatalogDB`, or its file is missing;
- the SD card is not mounted;
- NexusPlayer reports an error for the alarm song;
- **watchdog:** the song has not advanced its position within 3 s of starting.
  This catches a pipeline that is silent without reporting an error.

The switch is one way for that ring: the alarm does not go back to the song.

### Scheduling (step A)

Until step B brings repeat days, the scheduler keeps the `hour:minute` alarms:

- The task sleeps until the next minute boundary (not a 5 s poll), so an alarm
  fires within about 100 ms of `:00`.
- Each alarm fires at most once per local minute; the "last fired" key is the local
  date and minute, held in the service (not a function `static`).
- No alarms fire until time is synced (year > 2020), and the log says so once. The
  time zone is the one from settings (`TimeSyncHelper::applyTimezone`).
- An alarm whose minute passed while the device was off or rebooting is skipped,
  not rung late.
- `tone_file` values that are WAV paths (the old default) mean "built-in tone";
  anything else is looked up as a `CatalogDB` id.

### sysdb and the dashboard channel

The `Alarm` component (sysdb) grows from `playing` / `stop_requested` /
`active_alarm_id` to:

| Field | Meaning |
|---|---|
| `state` | `IDLE`, `RINGING`, `SNOOZED` |
| `active_alarm_id` | alarm that is ringing or snoozed (0 = a test ring) |
| `using_builtin` | fell back to the built-in tone |
| `snooze_until` | epoch seconds, while snoozed |
| `stop_requested`, `snooze_requested` | writable request bits (keys, API) |

`playing` stays as an alias of `state == RINGING` for the existing readers (Key 2,
the wake-word gate). `ControlChannel` already pushes the `alarm` object to the
dashboard; it gets the new fields.

### Routes in step A

Only what is needed to test ringing (the full set is step C):

- `POST /api/alarms/ring` `{"tone"?}`: rings now as a test (id 0), same path as a
  real alarm.
- `POST /api/alarms/snooze`
- `POST /api/alarms/stop` (exists)
- `GET /api/alarms/status`: state, alarm id, tone, built-in fallback, time left.

## Reminders

A reminder is "at this time, tell me this": "remind me to call the bank tomorrow at
5", "every Monday at 9, standup".

### Shared with alarms

- **When:** a local time plus either a date (one-shot) or repeat days. One portable
  `nextFire(when, last_fired, now)` serves both, host-tested once (repeat days,
  midnight, DST, a date in the past).
- **Scheduler:** the same `AlarmService` task, minute-boundary wake-up, "fires once
  per minute", "missed while off is skipped". The service becomes the scheduler for
  both kinds; ringing stays alarm-only.
- **Storage (step B):** two collections in system.ndb, `alarms` and `reminders`,
  each holding the shared `when` fields plus its own: an alarm has tone, snooze and
  volume; a reminder has its text. Two collections rather than one with a `kind`
  field, so neither carries the other's empty fields and each can change on its own.

### Delivery

A reminder is an interruption, not a takeover:

1. The **`reminder` chime** (a new alert, so it gets a slot in the Sounds card and
   a custom sound like the others) plays through the alert mixer, ducking music.
2. The device **speaks the text**. Speech comes from a Gemini Live session opened
   by the reminder: the device sends the text as the first turn ("Reminder: call
   the bank") and Gemini says it. Music pauses for the voice, as it does for any
   session. The session then stays open for the normal follow-up window, so "done"
   or "remind me again in ten minutes" just works through the voice tools.
3. **Offline** (no Wi-Fi, or the session fails): the chime plays three times, and
   the reminder stays pending on the dashboard. Speaking offline (speech generated
   when the reminder is created and stored on the SD card) is a later phase.

A fired reminder is marked done (one-shot) or scheduled for its next day
(repeating). One that could not be spoken stays **pending** until it is
acknowledged on the dashboard, by voice, or by a key press, so a reminder is never
silently lost.

### Conflicts

| Going on | Reminder does |
|---|---|
| Alarm ringing or snoozed | Waits; delivered after the alarm stops. |
| Assistant session | Chimes now; the text is sent into the open session after the current turn ends, instead of opening a new one. |
| Music | Chime ducks it; voice pauses it; it resumes after the session, as usual. |
| Two reminders in the same minute | One chime, one session, both texts. |

### Timers

"Set a timer for 20 minutes" is a one-shot alarm whose date and time are now + 20
minutes, flagged so the dashboard shows a countdown. It rings through the alarm path
and needs no scheduler work of its own; the voice tool comes with step C.

## Later: briefing alarms

A briefing alarm (like Bixby's): at the alarm time the device plays a short spoken
briefing (news, weather, calendar) produced by a server, then music.

How it fits:

- **Pre-fetch, never fetch at ring time.** At fire time − 10 min, `AlarmService`
  asks a briefing provider (server URL in settings) for the audio (Ogg or WebM
  Opus, the formats we decode) and saves it to
  `/sdcard/media/alarm/briefing-<id>.ogg`. Ring time depends only on the SD card.
- **The program is `[briefing, music]`.** The briefing is an alarm-owned item
  (not looped). "Music" is handed to `MusicPlaybackService` as ordinary playback (a
  library song, playlist or search), so queue, autoplay and the dashboard work as
  usual and the alarm ends when the briefing ends and the music starts.
- **Failures degrade:** no briefing file → skip to music; no network for a stream →
  a library song; nothing playable → built-in tone.

Nothing here is built in step A; A only keeps the item list and the "alarm-owned
item" vs "handed to music" split.

## Steps

| Step | What |
|---|---|
| **A: ringing** | This document's *Ringing* section: `AlarmRing` (portable, host-tested), NexusPlayer owner mode, orchestrator priority, built-in alarm tone with loop, takeover (session, chimes, keys, volume), minute-boundary scheduler, sysdb fields, test routes. Alarms still come from `alarms.json`. |
| **B: storage and reminders** | `alarms` and `reminders` collections in system.ndb (shared `when`: time, date or repeat days, label, enabled; alarm: tone, snooze minutes, volume; reminder: text, pending). `nextFire` in portable code with host tests. `alarms.json` migrated once, then retired. Snooze, "last fired" and pending reminders survive a reboot. Reminder delivery: the `reminder` chime, speaking through a Gemini session, offline fallback, conflicts table. |
| **C: routes and voice** | `/api/alarms` and `/api/reminders` create/edit/delete, acknowledge; Gemini tools: `set_alarm` / `stop_active_alarm` updated (days, label, tone by song name), snooze, `set_reminder` / `list_reminders` / `cancel_reminder`, `set_timer`. Fix the dashboard library listing (Music → Library) that does not load, since tones come from it. |
| **D: dashboard** | Alarms page: alarms (editor with repeat days, tone picker with preview, test ring), reminders (text, date or repeat days, pending ones to acknowledge), running timers, and a ringing banner with Stop and Snooze. |

## Step A: measurements

Every check runs on the device, with logs from `/api/logs`:

1. **Fire to first sound** for a library song and for the built-in tone, idle
   (target: under 1 s from `:00`; the built-in within 50 ms of the request).
2. **Ringing over music:** local file and stream. After stop, the same track
   resumes within ±2 s of where it was; the queue has not moved.
3. **Ringing during a Gemini session:** the session ends, no chime plays over the
   alarm, wake word re-arms after stop.
4. **Fallbacks:** tone not in the catalog, file deleted, SD card unmounted, each
   rings the built-in tone within 3 s.
5. **Snooze and timeout** (shortened limits in a test build or via the test route):
   snooze rings again on time; timeout stops and restores.
6. **Keys:** Key 2 stops; Key 3/4/5 snooze and do not change volume or playback.
7. **Leaks:** 20 test rings back to back; internal heap and PSRAM free are the same
   before and after; no `Decode task did not stop` warnings; no underruns.
