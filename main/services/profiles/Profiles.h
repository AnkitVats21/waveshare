#pragma once
// Assistant profiles: named personas (voice, instructions, optional model)
// in system.ndb. One is active, and every session is set up from it. Its
// voice and instructions are mirrored into the old settings fields
// (gemini_voice, gemini_system_prompt), so older firmware keeps the
// persona. Memory, skills and the other settings are shared by all.
// See docs/plans/assistant-profiles.md.

#include "services/storage/SystemDatabase.h"

#include <ArduinoJson.h>

#include <string>
#include <utility>
#include <vector>

namespace Services::Profiles {

constexpr size_t MAX_PROFILES = 8;
constexpr size_t MAX_NAME = 32;
constexpr size_t MAX_VOICE = 32;
constexpr size_t MAX_MODEL = 96;
constexpr size_t MAX_INSTRUCTIONS = 8 * 1024;

// After openSystemDb() (which repairs a missing active id): publishes the
// active profile's name (sysdb assistant.profile).
void begin();

int activeId();
// The active profile; empty fields (the firmware defaults) if none.
ndb::system::ProfileDoc active();
// Every profile's id and name, sorted by id.
std::vector<std::pair<int, std::string>> names();

// Creates a profile (id 0; `id` is then set to the new one) or replaces
// one. Editing the active profile's voice, instructions or model starts a
// new conversation at the next session. False with `error` set.
bool save(int& id, const ndb::system::ProfileDoc& doc, std::string& error);
// Deleting the active profile activates the default. The default can't be
// deleted. False with `error` set.
bool remove(int id, std::string& error);
// The next session uses it, in a new conversation. False if unknown.
bool activate(int id);

// For the system instruction: which profile the model is, and the names it
// can switch to. "" with a single profile.
std::string instructionsLine();

// The switch_profile tool: activates the profile `name` means and, from a
// live session, restarts the session once the reply is spoken.
void switchTool(const std::string& name, JsonDocument& response);

} // namespace Services::Profiles
