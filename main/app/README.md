# main/app

App logic on top of the components: what the keys do, how audio services
start and stop, the LED, and the recorder.

## AppController

Receives Gemini's decoded tool calls and sends the reply back:
`gemini_live`'s `DeviceCommandHandler` (volume, LED, notes, memory, and
alarm tools through `IDeviceCommandDelegate::handleAlarmTool`, which
`AppController` implements with `services/alarm/AlarmTools`) is tried
first, then `MediaCommandHandler` (music). After a music command the
session ends as soon as Gemini finishes speaking, rather than waiting for
the follow-up window.

## audio/

- **`AudioService`** — a `ReactorTask` watching audio settings (volume, mic
  gain, sample rate) and the pipeline mode; owns the wake-word engine and
  the speaker playback task. It is also the `IWakeWordListener` that starts
  an assistant session.
- **`recording/`** — the recorder:
  - `AudioRecorder`: records to `/sdcard/recordings` as Ogg Opus. Modes:
    **stereo** (both mics before processing, 24 kHz 48 kbps, or 16 kHz 32 kbps
    if music is playing when it starts, to save CPU) and **processed** (the
    AFE's mono output, what the wake word hears, 16 kHz 24 kbps). Wake word
    is suppressed while recording; playback continues. Stops by itself
    after 10 minutes. Files are named `rec_YYYYMMDD_HHMMSS_<mode><rate>k.opus`
    (or `rec_NNN_<mode>.opus` before the clock is set). On stop it adds the
    file to `recordings.ndb`.
  - `OggOpusEncoder` (behind `IRecordingEncoder`): 1 s Ogg pages (50
    packets of 20 ms), header pages alone, fsync every 10 pages.
  - `PolyphaseResampler`: 32 kHz mic feed to the encode rate.
  - `RecordingProbe`: length, rate and channels of an existing file (Ogg:
    OpusHead plus the last page's granule; WAV: fmt and data chunks). Used
    by the boot check of `recordings.ndb`. Host-tested.

## input/ — keys

`KeyService` polls the five keys every 20 ms (long press = 500 ms):

| Key | Press | Long press |
|---|---|---|
| 2 | start a processed recording, or stop the active one | start a stereo recording |
| 3 | volume up | next track |
| 4 | play / pause | |
| 5 | volume down | previous track |

While an alarm rings or is snoozed, key 2 stops it and the others snooze.
Key 1 has no action.

## led/

`LedService` animates the LED ring from sysdb (`led.mode`, colour, speed)
and the assistant/player state.
