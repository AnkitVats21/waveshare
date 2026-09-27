# tools

| Tool | What |
|---|---|
| [`starc/`](starc/README.md) | Schema compiler: `sysdb.star` → `SystemState`, `db/*.star` → nexus_db classes and the dashboard's JS schema |
| `webbundle/mkbundle.py` | Packs a built frontend (Vite `dist/`) into a web bundle for the `www_0`/`www_1` flash slots; `--upload http://<device>` sends it to `/api/ota/frontend` (the dashboard's `npm run deploy` calls it) |
| `voice_test/` | `converse.py` wakes the device over the API and speaks prompts through the PC speakers (`speak.py`, pre-generated TTS, since the device ends a session after ~3 s of silence), then prints what it heard and answered. Device address from `NEXUS_DEV` |
| [`lvgl_sim/`](lvgl_sim/README.md) | Desktop LVGL simulator for the `ui_view` screens, fed from the device's HTTP API |
