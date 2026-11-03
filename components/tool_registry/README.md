# tool_registry

The tools the voice model can call, and the one path every call and every
answer takes. Portable C++ (ArduinoJson only); routing is tested on the host
(`host_tests/tests/test_tool_registry.py`).

## How it works

- A feature registers its tools at startup (`main/main.cpp` wires them):
  name, kind (`search`, `music`, `bluetooth`, `info`, `device`, `other`), and
  a handler. Adding a tool touches the schema and the feature's own file.
- The voice backend hands each call to `Registry::dispatch()`. A tool from
  the schema gets its arguments decoded and checked first (a missing
  required argument is answered with an error and the handler isn't
  called). A name nobody registered goes to the remote source (the MCP
  server); an unknown name is answered with an error.
- A handler answers now (fills the reply) or later (returns
  `Answer::Later` and calls `registry->answer(id, json)` from its worker).
  Every answer goes through `answer()`, so the registry sees each tool
  start and end, and tells its observers (`Event`: name, kind, remote,
  status, elapsed ms). `busy()` says a call is still waiting for its answer.
- Status of an answer: its `status` field, `success`, `ambiguous`,
  `not_found`, anything else is an error.
- The session setup's declarations come from `declareLocal()` (the
  registered tools, in schema order, plus any with their own declaration)
  and `declareRemote()`.

## The schema

`schema/gemini_skills_schema.json` holds the declarations of the board's
own tools. `scripts/generate_gemini_skills.py` (run by the build) turns it
into `skills_generated.{h,cpp}`: the declarations as JSON and a typed
argument struct and decoder per tool. Edit the schema, not the output.

## Who registers what

| Tools | Kind | Where |
|---|---|---|
| `play`, `playback`, `volume`, `music_settings`, `music_output` | music | `media_player/src/MusicTools.cpp` |
| `notes`, `save_to_memory`, `set_led_strip` | device | `main/services/tools/DeviceTools.cpp` |
| `set_alarm`, `set_timer`, `schedule`, `list_schedule`, `cancel_scheduled`, `ringing_alarm`, `acknowledge_reminders` | device | `main/services/alarm/AlarmTools.cpp` |
| `get_weather` (answers later) | info | `main/services/weather/Weather.cpp` |
| `switch_profile` | device | `main/services/profiles/Profiles.cpp` |
| the MCP server's tools (remote) | search for `*search*`/`*news*`, else other | `main/services/mcp/McpService.cpp` |

In hub mode the hub declares its own music and Bluetooth tools under the
same names and runs them itself; the board's declarations of those are
dropped by the hub.
