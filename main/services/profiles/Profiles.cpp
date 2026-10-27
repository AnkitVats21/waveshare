#include "services/profiles/Profiles.h"

#include "services/profiles/ProfileMatch.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "gemini_live/AssistantService.h"
#include "gemini_live/VoiceAgent.h"
#include "esp_log.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace Services::Profiles {

namespace {

const char* const TAG = "Profiles";

using ndb::system::ProfileDoc;
using ndb::system::Settings;

std::mutex s_mutex;              // save, remove, activate
std::atomic<int> s_active{0};

void publishName(const std::string& name) {
    EmbeddedSysDb::getInstance().mutate([&name](SystemState& s) {
        strncpy(s.assistant.profile, name.c_str(), sizeof(s.assistant.profile) - 1);
        s.assistant.profile[sizeof(s.assistant.profile) - 1] = '\0';
    });
}

// The old fields follow the active profile (for a firmware rollback). The
// global model is not touched: a profile's empty model means that one.
bool storeActive(int id, const ProfileDoc& doc) {
    Settings s;
    s.active_profile = uint16_t(id);
    s.gemini_voice = doc.voice;
    s.gemini_system_prompt = doc.instructions;
    if (!saveSettings(s, Settings::F_ACTIVE_PROFILE | Settings::F_GEMINI_VOICE | Settings::F_GEMINI_SYSTEM_PROMPT)) {
        return false;
    }
    s_active = id;
    publishName(doc.name);
    return true;
}

bool activateLocked(int id) {
    ProfileDoc doc;
    if (!loadProfile(id, doc)) return false;
    if (!storeActive(id, doc)) return false;
    VoiceAgent::newConversation();
    ESP_LOGI(TAG, "Active profile: %d '%s'", id, doc.name.c_str());
    return true;
}

std::string trimmed(const std::string& in) {
    size_t a = in.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = in.find_last_not_of(" \t\r\n");
    return in.substr(a, b - a + 1);
}

}  // namespace

// Reads only: openSystemDb() has made sure the active id names a profile.
// Runs on the small main task.
void begin() {
    int id = loadSettings().active_profile;
    ProfileDoc doc;
    if (!loadProfile(id, doc)) {
        ESP_LOGW(TAG, "No profiles (system.ndb not open?); using the settings");
        return;
    }
    s_active = id;
    publishName(doc.name);
    ESP_LOGI(TAG, "Active profile: %d '%s'", id, doc.name.c_str());
}

int activeId() {
    return s_active.load();
}

ProfileDoc active() {
    ProfileDoc doc;
    if (loadProfile(s_active.load(), doc)) return doc;
    // No profiles (no card): the settings as before profiles.
    Settings s = loadSettings();
    doc.voice = s.gemini_voice;
    doc.instructions = s.gemini_system_prompt;
    return doc;
}

std::vector<std::pair<int, std::string>> names() {
    std::vector<std::pair<int, std::string>> out;
    for (auto& [id, doc] : listProfiles()) out.emplace_back(id, std::move(doc.name));
    return out;
}

bool save(int& id, const ProfileDoc& in, std::string& error) {
    ProfileDoc doc = in;
    doc.name = trimmed(doc.name);
    doc.voice = trimmed(doc.voice);
    doc.model = trimmed(doc.model);
    if (doc.name.empty() || doc.name.size() > MAX_NAME) {
        error = "name must be 1-32 characters";
        return false;
    }
    if (doc.voice.size() > MAX_VOICE || doc.model.size() > MAX_MODEL) {
        error = "voice or model is too long";
        return false;
    }
    if (doc.instructions.size() > MAX_INSTRUCTIONS) {
        error = "instructions are limited to 8 KB";
        return false;
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    auto existing = names();
    for (const auto& [other, name] : existing) {
        if (other != id && ProfileMatch::sameName(name, doc.name)) {
            error = "another profile has this name";
            return false;
        }
    }
    ProfileDoc old;
    if (id == 0) {
        if (existing.size() >= MAX_PROFILES) {
            error = "at most 8 profiles";
            return false;
        }
        id = nextProfileId();
    } else if (!loadProfile(id, old)) {
        error = "no such profile";
        return false;
    }
    if (!saveProfile(id, doc)) {
        error = "could not save the profile";
        return false;
    }
    if (id == s_active.load()) {
        storeActive(id, doc);
        if (doc.voice != old.voice || doc.instructions != old.instructions || doc.model != old.model) {
            VoiceAgent::newConversation();
        }
    }
    ESP_LOGI(TAG, "Saved profile %d '%s'", id, doc.name.c_str());
    return true;
}

bool remove(int id, std::string& error) {
    if (id == DEFAULT_PROFILE_ID) {
        error = "the default profile can't be deleted";
        return false;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    ProfileDoc doc;
    if (!loadProfile(id, doc)) {
        error = "no such profile";
        return false;
    }
    if (id == s_active.load() && !activateLocked(DEFAULT_PROFILE_ID)) {
        error = "could not activate the default profile";
        return false;
    }
    if (!removeProfile(id)) {
        error = "could not delete the profile";
        return false;
    }
    ESP_LOGI(TAG, "Deleted profile %d '%s'", id, doc.name.c_str());
    return true;
}

bool activate(int id) {
    std::lock_guard<std::mutex> lock(s_mutex);
    return activateLocked(id);
}

std::string instructionsLine() {
    auto all = names();
    if (all.size() < 2) return {};
    std::string line = "Assistant profile: you are \"";
    std::string others;
    for (const auto& [id, name] : all) {
        if (id == s_active.load()) {
            line += name;
            continue;
        }
        if (!others.empty()) others += ", ";
        others += name;
    }
    line += "\". The user can switch you to another profile with switch_profile: " + others + ".";
    return line;
}

void switchTool(const std::string& name, JsonDocument& response) {
    auto all = names();
    std::vector<std::string> list;
    for (const auto& entry : all) list.push_back(entry.second);
    int index = ProfileMatch::match(list, name);
    if (index < 0) {
        response["status"] = "error";
        response["message"] = index == ProfileMatch::AMBIGUOUS
                                  ? "Several profiles match; ask the user which one."
                                  : "No profile by that name; tell the user the available ones.";
        JsonArray profiles = response["profiles"].to<JsonArray>();
        for (const auto& n : list) profiles.add(n);
        return;
    }
    const auto& [id, picked] = all[index];
    response["profile"] = picked;
    if (id == s_active.load()) {
        response["status"] = "success";
        response["message"] = "This profile is already active.";
        return;
    }
    if (!activate(id)) {
        response["status"] = "error";
        response["message"] = "Could not switch the profile.";
        return;
    }
    // The voice and instructions are fixed for a connection: the session
    // starts again once this reply is spoken.
    if (EmbeddedSysDb::getInstance().snapshot().assistant.session_state != AssistantState::Idle) {
        AssistantService::requestRestart(uint32_t(loadSettings().manual_silence_s) * 1000);
    }
    response["status"] = "success";
    response["message"] = "Switched. Confirm in a few words; the conversation then starts afresh in the new "
                          "profile's voice.";
}

} // namespace Services::Profiles
