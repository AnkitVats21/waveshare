# http_server

App-agnostic pieces of the device web server. The app's routes live in
`main/services/http/routes`; this component knows nothing about them.

- **`Http::Server`**: owns an `esp_http_server` instance; `on()` /
  `onWebSocket()` register routes and log registration failures (e.g. when
  `max_uri_handlers` is exhausted). With wildcard URI matching the first
  registered match wins, so catch-alls go last.
- **`HttpUtil.h`**: `sendJson` / `sendError` / `sendOk` (CORS headers,
  `Connection: close`), `readBody` (loops over partial receives, size cap),
  `queryParam` (URL-decoded), `corsPreflight`.
- **`Http::WebBundle`**: serves a web frontend from flash. The bundle
  (format documented in `WebBundle.h`, built by `tools/webbundle/mkbundle.py`)
  lives in one of two data partitions `www_0` / `www_1`; the active slot is
  stored in NVS (`www/slot`). Files are served from the memory-mapped
  partition with `Content-Encoding: gzip`, ETags, and long-lived caching for
  hashed `/assets/*`. Extension-less unknown paths get `/index.html`.

Flash writes, partition mmap and NVS writes freeze the cache and must not run
on a task with a PSRAM stack (the httpd task's stack is in PSRAM here), so
`WebBundle::init()` runs from `app_main` and `activate()` / `info()` run on
an internal-stack worker (`main/services/http/FlashUpload`). `serve()` only
reads already-mapped memory and is safe on the httpd task.
