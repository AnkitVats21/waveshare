# credentials

Secrets in NVS, never in files on the SD card, never logged.

| NVS key | What |
|---|---|
| `creds/gemini_api_key` | Gemini API key |
| `wifi_store/ssid`, `wifi_store/password` | Wi-Fi station credentials |

API: `geminiApiKey()`, `setGeminiApiKey()`, `loadWifi()`, `saveWifi()`,
`clearWifi()`.

## Provisioning from the SD card

`importFromSdCard()` runs at boot, after NVS and the card are up:

- `/sdcard/wifi_config.json` (`{"ssid", "password"}`): imported, then the
  file is deleted.
- `/sdcard/gemini_config.json`: `api_key` imported, then removed from the
  file (other fields stay).
- `/sdcard/settings.txt` (older cards): `wifi.*` and `mqtt.*` lines dropped.

A value is removed from the card only after it reads back from NVS. So a
fresh device is set up by dropping the JSON files on the card and booting,
or through the setup access point and captive portal (Wi-Fi), or with
`POST /api/config/gemini` (`api_key`) and `POST /api/wifi/configure`. The HTTP file API refuses both JSON files.

`idf.py erase-flash` wipes the credentials; `app-flash` and OTA keep them.
