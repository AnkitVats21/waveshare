# main/hal

Board bring-up for the Waveshare ESP32-S3 audio board. `Board` owns the
hardware objects, initialises them in dependency order and hands typed
references to the services (`board.getAudio()`, `getLeds()`,
`getIoExpanderInstance()`); services never create hardware objects
themselves.

| Folder | What |
|---|---|
| `Board`, `Board_defs.h` | Order of bring-up, pin and bus definitions, SD card mount |
| `audio/AudioHal` | I2S in and out, the ES7210 mic ADC and ES8311 speaker codec (`esp_codec_dev`), and the raw mic feed the AFE reads (`IAudioFeedSource`) |
| `io/` | I2C bus, TCA95xx 16-bit IO expander (keys, amplifier and power-rail enables) |
| `input/ExpanderKeyInput` | Key states read through the expander |
| `led/LedStripManager` | The WS2812 LED ring (`espressif__led_strip`) |
| `display/LcdManager` | The optional LCD and touch (`CONFIG_DISPLAY_ENABLE`), driving `ui_view` |
| `network/WifiService.h` | Forwarding header; Wi-Fi lives in `services/network` |

Audio runs at `LOCAL_SAMPLE_RATE` (32 kHz). The mics give 4 interleaved
channels (the AFE format string says which are mics and which is the
playback reference for echo cancellation).
