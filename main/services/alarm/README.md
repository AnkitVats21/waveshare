# main/services/alarm

Alarms, timers and reminders. The design, with the reasons behind it, is
`docs/alarm-design.md`; this is the map of the code.

| File | What |
|---|---|
| `AlarmSchedule` | When an alarm or reminder next fires: repeat days, DST gaps and repeats, snooze. Portable, host-tested (`test_alarm_schedule.py`) |
| `AlarmRing` | What a ringing alarm does, as a state machine with no I/O: it returns actions (`playItem`, `playBuiltin`, `silence`, `restoreMedia`). Host-tested (`test_alarm_ring.py`) |
| `AlarmService` | Runs it on the device: the minute-boundary scheduler, takes over the player (`NexusPlayer` alarm owner mode) or plays the built-in tone, stops an assistant session and chimes, restores the music afterwards, publishes `alarm` state to sysdb |
| `AlarmTools` | Gemini tools: `set_alarm`, `set_timer`, `set_reminder`, `list_schedule`, `cancel_scheduled`, `ringing_alarm`, `acknowledge_reminders` |

- Alarms and reminders are stored in `system.ndb` (collections `alarms`,
  `reminders`), keyed by a positive integer id. A snooze survives a restart.
- An alarm's tone is a library song (played from the card through the
  player) or the built-in tone, which is synthesised into PSRAM and never
  needs the card. A song that can't play falls back to the built-in tone.
- HTTP: `/api/alarms`, `/api/reminders`, `/api/alarms/status`; the keys
  stop or snooze a ringing alarm (see `main/app/README.md`).
