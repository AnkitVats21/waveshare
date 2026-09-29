# Plan: assistant profiles

**Status:** agreed on 2026-09-29, not built yet.

## Goal

Several named assistant personas, for example "Yoga teacher" or "Technical
companion". You switch between them from the dashboard or by voice.

## What a profile holds

| Field | Notes |
|---|---|
| name | Shown in the dashboard and spoken in voice switching; unique |
| voice | A prebuilt voice name; empty means the firmware default |
| instructions | The system prompt of this persona |
| model | Optional; empty means the global model |

**Shared by all profiles:** memory (`gemini_memory.txt`), notes, skills
(built-in and MCP), the API key, and the Advanced settings (session timing,
voice detection, transcripts). What the assistant knows about the user
stays the same across personas.

## Firmware

1. **Storage.**
   - A `profiles` collection in `schema/db/system.star` holds name, voice,
     instructions and model.
   - `settings.active_profile` holds the active profile's id.
   - **Migration at boot:** with no profiles yet, the current
     `gemini_voice` and `gemini_system_prompt` become a profile called
     "Default", and it is made active. The old fields stay, so rolling back
     the firmware keeps working.
2. **Session setup.** `SessionSettings` (see `main.cpp`) takes the voice,
   instructions and model from the active profile. The global model is used
   when the profile leaves it empty.
3. **REST.**
   - `GET /api/assistant/profiles` returns the list and the active id.
   - `POST /api/assistant/profiles` creates or updates a profile (with `id`).
   - `DELETE /api/assistant/profiles?id=` deletes one. Deleting the active
     profile activates Default. Default itself can't be deleted.
   - `POST /api/assistant/profiles/activate` makes a profile active.
4. **Voice switching.** A tool `switch_profile(name)`:
   - The name is matched case-insensitively. When nothing matches, the tool
     returns the available names so the model can ask.
   - The profile is fixed at setup, so the switch happens right away: the
     confirmation is spoken, then the session reconnects with the new
     profile. Use the existing restart path (as `requestRestart`) after the
     reply's `turn_complete`.
   - The new session starts fresh. Resumption must not carry a conversation
     across profiles: drop the resume handle when the profile changes.
   - The profile names go into the instructions (one short line), so "the
     yoga one" resolves.
5. **Status.** `/api/assistant/status` and the WS snapshot carry the active
   profile's name.

## Dashboard

- **Assistant → Personality** becomes a profile list (name, voice, an
  "Active" badge), with an editor for the fields above, plus Activate and
  Delete.
- **Header pill:** "Waiting for wake word · Yoga teacher".
- The `ndb_schema.js` regeneration picks up the new collection.

## Tests

- Host test for the name matching (exact, case, partial, ambiguous).
- **On the device:**
  - switch by voice mid-conversation: measure the gap, and check the new
    voice and that the context is fresh;
  - switch from the dashboard between sessions;
  - delete the active profile;
  - migrate from a settings file with a prompt and a voice.

## Later

- A backend field per profile, if a second voice backend (e.g. OpenAI
  Realtime) is added. Voice names are backend-specific.
- An LED colour per profile.
