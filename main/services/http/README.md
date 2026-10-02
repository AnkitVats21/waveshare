# main/services/http

The device's web server: the REST API, the `/api/ws` control channel, the
dashboard bundle, the built-in recovery page and the setup portal. The
server itself (`Http::Server`, JSON helpers, the bundle from flash) is the
`http_server` component; this folder holds the app's routes.

| File | What |
|---|---|
| `HttpService` | Starts the server (80 URI handler slots, ~65 used), registers the route groups |
| `ControlChannel` | `/api/ws`: one client at a time (a new one takes over); full state on connect, then on every relevant sysdb change (coalesced) and periodically; commands as `{"cmd": ...}`; opt-in transcript stream. Advertises `nexus.local` over mDNS |
| `routes/*Routes.cpp` | One file per area, registered from `Routes.h` |
| `FlashUpload` | OTA and bundle writes on an internal-RAM stack (the httpd task's stack is in PSRAM, and flash writes freeze the cache) |
| `SystemInfo`, `StateNames.h` | Facts and names shared by the REST routes and the WebSocket push |
| `web/` | Built-in pages compiled into the firmware: recovery dashboard, captive portal |

Every reply carries CORS headers (the dashboard may be served from a PC
during development). Errors are `{"status":"error","message":...}` with a
matching HTTP status.

## Routes

| Area | Routes |
|---|---|
| Music | `POST /api/music/play` (`stream_url`, `id`, `title`, `artist`, `duration`), `POST /api/music/play_local` (`id`), `POST /api/music/control` (`action`: pause, resume, toggle, stop, next, prev, seek, repeat, autoplay, caching, shuffle, clear_queue; `value`), `GET /api/music/status`, `POST /api/music/library/scan` (checks the library against the card), `DELETE /api/music/library?id=` (deletes the saved file; the entry stays); the library is `GET /api/db/music` |
| Recordings | `POST /api/recordings/play` (`id`), `POST /api/recordings/rename` (`id`, `name`), `DELETE /api/recordings?id=`; the list is `GET /api/db/recordings`; recording itself: `POST /api/audio/record/start?mode=stereo\|processed`, `POST /api/audio/record/stop` |
| Assistant | `POST /api/assistant/wake` (optional body `{"text": ...}`: sent as the person's turn), `/start`, `/stop`, `/handoff` (test: move the session to a new connection, as after a goAway), `GET /api/assistant/status`, `GET /api/assistant/transcript` |
| Audio | `POST /api/audio/volume`, `/mic_gain`, `/mic_mute`, `/alert` |
| Alerts (chimes) | `GET /api/alerts`; `POST /api/alerts/<name>` (enabled, gain, source), `/<name>/upload`, `/<name>/play`, `/<name>/reset` |
| Alarms | `GET/POST/DELETE /api/alarms`, `POST /api/alarms/*`, `GET /api/alarms/status`; `GET/POST/DELETE /api/reminders`, `POST /api/reminders/*` |
| Files | `GET /api/files?path=` (list), `GET /api/files/download?path=` (Range requests: 206 / 416), `POST /api/files/upload?path=` (raw body), `POST /api/files/mkdir`, `POST /api/files/rename`, `DELETE /api/files?path=`. Paths go through `sd_storage::PathPolicy`; the credential files are refused |
| Databases | `GET /api/db/<name>` (`system`, `recordings`, `music`): the raw `.ndb` file |
| Config | `GET/POST /api/config/gemini` (model, voice, prompt, transcript and session options, web_search, weather_location, vad_*; the key is write-only), `GET /api/config/gemini/models` (voice models this key can use, from Google, with whether search works on each), `GET/POST /api/config/settings` (timezone, …) |
| MCP | `GET /api/mcp` (status, tools list, last error), `POST /api/mcp` (`url`, `token`, `max_tools`), `POST /api/mcp/refresh` (trigger background tools refresh) |
| Time | `GET/POST /api/time`, `POST /api/time/sync` |
| Wi-Fi | `GET /api/wifi/status`, `GET /api/wifi/scan`, `POST /api/wifi/configure` |
| System | `GET /api/system/metrics`, `/init`, `/delta` (for the built-in page), `GET /api/system/tasks` (each task's stack: size, least free, internal or PSRAM), `GET /api/system/flash` (partitions: size and what is in use, e.g. firmware length and version), `GET /api/storage/info`, `GET /api/logs?since=<seq>` (~80 lines), `POST /api/led/set`, `POST /api/system/reboot`, `GET/POST /api/system/usb-mode` (`{"mode":"serial"|"ethernet"}`: the USB port as serial with Wi-Fi, or as a USB network adapter with Wi-Fi off; saving reboots), `GET /api/system/nettest?url=&max=[&quiet=1]` (download speed from the board), `POST /api/system/nettest[?quiet=1]` (upload speed to the board; both report receive pauses, IP precedence counts and lwIP drops), `GET/POST /api/system/wifi-mac` (`{"random":bool}`: a random station MAC, for tests; reboots), `GET/POST /api/system/wifi-rx` (`{"static","dynamic","ba_win","ampdu_rx","no_11b"}` or `{"reset":true}`: Wi-Fi receive settings for tests; reboots), `POST /api/system/wifi-log` (`{"level":0-5}`: Wi-Fi driver log level until reboot; its text reaches the serial console only), `POST/GET /api/system/wifi-sniff` (`{"seconds":1-30}` starts a radio capture of the frames the AP sends the board; GET returns rates, aggregation, retries per TID and Block Ack requests), `GET /api/system/tuning` (performance-experiment flags from `/sdcard/tuning.json`: value, set by the file or default, read by the firmware) |
| OTA | `POST /api/ota` (firmware image as the body), `GET /api/ota/status`; `GET/POST /api/ota/frontend` (dashboard bundle), `POST /api/ota/frontend/rollback` |
| Pages | `GET /recovery` (built-in dashboard), `GET /setup` (captive portal), `GET /*` (the dashboard bundle; the portal while the setup access point is up) |
| Control | `/api/ws` (WebSocket) |

The dashboard bundle lives in two flash slots (`www_0`, `www_1`); an
upload goes to the inactive slot and switches over, so a bad bundle can be
rolled back, and `/recovery` works even with no bundle.
