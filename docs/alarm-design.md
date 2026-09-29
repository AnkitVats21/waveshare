# Alarms and reminders

Alarms that actually ring. When an alarm fires, everything else stops: the assistant
session ends, chimes go quiet and music is put aside. The device plays only the
alarm until someone stops or snoozes it, then puts the music back.

Reminders share the scheduler: same "when", different delivery. A reminder does not
take over the device; it chimes and speaks its text over whatever is going on (see
*Reminders*).

Status: steps A–D built (2026-09-27); briefing alarms built (2026-09-29, see
*Briefing alarms*).

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

### Tones

`AlarmDoc::tone` (parsed by `parseTone`, `AlarmSchedule.h`):

| Setting | Plays |
|---|---|
| `""`, `builtin`, `builtin:<name>` | A synthesized pattern: `classic` (default), `chime`, `digital`, `rising`. Rendered into the alarm slot when the alarm rings. |
| `file:<name>` | An Opus file (`.ogg`, `.opus`, `.webm`; no MP3 decoder) uploaded to `/sdcard/media/alarm` with `/api/files/upload`, played by path. |
| anything else | A library song id. A YouTube video becomes one with `POST /api/alarms/tones/youtube`, which downloads it into the music cache without playing it. |

Whatever can't play falls back to the classic built-in tone, as above.
`GET /api/alarms/tones` lists the patterns and uploaded files. By voice,
`set_alarm`'s `tone` is matched against the pattern names, then the file names,
then the library.

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

**Scheduled actions.** The same store holds actions (`action = true`, the
`schedule` tool with `kind: action`): "play lofi at 7", "at 10 pm set the volume
to 20", "run the routine good night" (steps in the note `routines`). When one is
due, the session opens without a chime and Gemini carries it out with its tools,
then confirms in a few words. An action is never pending: offline, it is skipped
and logged, since playing music an hour late is wrong. While Gemini answers a
due item (until that turn completes, at most 85 s), `schedule` and
`cancel_scheduled` refuse, so a recurring routine can't add or remove items
unattended.

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

## Briefing alarms (built)

A briefing alarm (`kind` 2) is its own type: it wakes the user with music
instead of a tone and gives a spoken morning briefing. Normal alarms ring
their tone and never brief. (Alarms with the old `briefing` switch are
converted at startup.)

**What it rings** (`briefingTone`, host-tested): its own `tone`
(`"file:<name>"` in `/sdcard/media/alarm`; nothing else is accepted), else the
`briefing_music` setting, else the soft built-in `rising` tone. A file that
can't play also falls back to `rising`. The music fades in over 30 s.

**`briefing_start`:**

| | after_stop (default) | automatic |
|---|---|---|
| Alarm time | the music (or soft tone) rings until stopped, snoozed or the ring limit | Gemini connects, then the music fades in (10 s); the briefing starts on its own |
| Briefing | once the user stops it: the music waits for the session, then comes back from where it stopped | over the music, ducked |
| After it | 8 s follow-up window, 60 s of music, 3 s fade | the music rises and rings as a normal alarm until stopped (or the ring limit) |
| User replies, wake word, stop | the music fades out in 1 s; normal conversation | the same, and the alarm won't ring after it |
| Snooze | as any alarm | ends the briefing; rings after the snooze and briefs when stopped |
| Offline | the ring is unchanged; no briefing | no briefing: it rings at once |

During an automatic briefing the alarm reports `ringing`, so Key 2 stops it
and the other keys snooze it, as while ringing. The volume floor (60) applies
from the alarm time.

The briefing itself is delivered like a scheduled action (see Reminders): the
session opens without a chime and Gemini is asked for a short briefing with
its tools (time, weather, `list_schedule`, headlines through MCP), spoken
slowly and leaving playback alone. A note called `briefing` overrides the
contents. Nothing is fetched before the alarm.

Settings: `briefing_music` (a file in `/sdcard/media/alarm`; `""` = none) and
`briefing_duck` (10–50 %, default 20), through `GET`/`POST /api/alarms/briefing`.
Uploads to the folder must be Opus (checked from the first bytes, `AudioSniff`).
API: `POST /api/alarms` with `"kind": "briefing"`, `"briefing_start"`; voice:
`set_alarm` with `type: briefing`, `briefing_start`. A test ring takes
`"briefing": true` and `"automatic": true`.

### Music phases

`/api/alarms/status` reports the phase as `briefing_music`.

| Phase | Music | Ends when |
|---|---|---|
| waiting | none yet: Gemini sets up first (at most 8 s; offline: at once) | Gemini's `setupComplete` |
| intro | playing; the tools run | the briefing's voice starts |
| under_voice | ducked to `briefing_duck` % over 500 ms, the voice held meanwhile; stays ducked for the 8 s follow-up window | no reply: the alarm service ends the session; or the user replies |
| tail | back to full over 2 s (after_stop) | 60 s pass, or a session starts (wake word) |
| fading | 0 over 3 s, then stopped | |
| stopping | 0 over 1 s, then stopped (a reply, the wake word, stop) | |
| after_session | none | the session is idle: the music from before the alarm resumes |

The alarm keeps `NexusPlayer` through all of it (focus events are ignored,
the music loops; `beginAlarm` again is a no-op, so the automatic briefing's
music becomes the ring without restarting) and gives it back at the end, so
the old music resumes where the alarm cut it. A music command takes the
player at once (no resume). `AudioOrchestrator::setMusicUnderVoice` keeps
sessions from pausing the music and chimes from changing its level.

Why the alarm service ends the follow-up window itself: the mic's voice
detection counts the ducked music as speech, so the session's own silence
timeout sometimes never came (the 60 s idle timeout ended it instead).

Why the music waits for `setupComplete`: a TLS handshake while the music
decodes took internal RAM to 7.7 KB. A session reports "listening" before
setup completes, and a refused setup (Google Search without quota on
Gemini 3.x) reconnects: waiting for "listening" still overlapped the second
handshake (8.6 KB).

Measured (2026-09-29, test rings): automatic: music 5.1 s after the alarm
time (with the search reconnect), briefing voice 14 s after, then the music
rang until stopped; internal RAM low point 15.9 KB. after_stop: the music
came back from 6.1 s where it was stopped, briefing 12.7 s after the stop,
follow-up closed after 8 s; 15.9 KB. A reply end was found 0.76 s after
`turnComplete` with music playing. One briefing never spoke: the weather
tool's response lost the WebSocket send lock (1 s) to the MCP response under
load and was dropped; tool responses are now retried (3 × 3 s).

## Earlier idea: pre-rendered briefing audio

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

## Step B: measured

On the device, 2026-09-27:

- **Migration:** the one `alarms.json` alarm came across as a daily alarm with
  the built-in tone (its `.wav` path dropped); the file is kept as `.bak`.
- **Timer:** rang 0.09 s after its second and was deleted.
- **One-shot alarm:** rang on the minute and was disabled after firing.
- **Snooze across a restart:** snoozed (1 min), rebooted; rang again on time
  from the saved `snooze_until`, which stop then cleared. While the saved
  snooze waited, `/api/alarms/status` said `idle`; fixed in `c9a48f2`, it now
  reports `snoozed` (alarm id, time left) and Stop cancels it.
- **Reminder, offline path:** chime three times 2.5 s apart; pending until
  acknowledged.
- **Reminder, spoken:** chime, session, text turn after `setupComplete`;
  Gemini started speaking 5.3 s after the reminder was due (1 s chime, 2 s
  connect, 1.5 s to the reply). The follow-up window works as usual.
- **Not yet checked:** the 25 s no-speech fallback, a reminder due during a
  ringing alarm or mid-turn, two reminders in one turn.

## Steps C and D: built

- **Voice (C):** Gemini tools `set_alarm` (repeat days as words, a day for a
  one-time alarm, label, tone by song name), `snooze_alarm`, `list_alarms`,
  `cancel_alarm`, `set_timer`, `set_reminder`, `list_reminders`,
  `cancel_reminder`, `acknowledge_reminders`. A voice alarm is one-time unless
  days are given; a repeated tool call updates the same alarm instead of adding
  another. Checked by voice: timer, weekday alarm, list, cancel, reminders for
  tomorrow and every Monday, a reminder in two minutes (spoken 4.5 s after due).
- **Routes (C):** `/api/alarms`, `/api/reminders`, `/api/alarms/{status,snooze,ring}`
  and `/api/time`.
- **Dashboard (D):** Alarms page with alarms (repeat days or a date, label,
  library song as tone with a 15 s test ring, snooze length, volume), timers
  (countdown, cancel), reminders (due time, acknowledge) and a clock card. The
  ringing banner offers Stop and Snooze, and shows a snoozed alarm with Stop.
  Home shows the next alarm, running timers and due reminders.
