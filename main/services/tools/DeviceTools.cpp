#include "services/tools/DeviceTools.h"
#include "core_sysdb/EmbeddedSysDb.h"
#include "esp_log.h"
#include "sd_storage/Fs.h"
#include "sd_storage/File.h"
#include "gemini_live/VoiceAgent.h"
#include <algorithm>
#include <cstring>
#include <ctime>

// Notes, the long-term memory file and the LED colour: the voice model's
// device tools.

static const char* TAG = "DeviceTools";

namespace {

// Notes are confined to one flat notes folder so the model
// can't read secrets (gemini_config.json) or overwrite device config.
constexpr const char* NOTES_DIR = "/sdcard/notes";
constexpr size_t NOTE_NAME_MAX = 64;
constexpr size_t MAX_DELETE_NOTES = 10;
constexpr size_t NOTE_READ_MAX = 8192;  // keeps the tool response small
constexpr size_t NOTE_LIST_MAX = 50;

// Maps a model-supplied name ("shopping.txt", or "/sdcard/notes/shopping.txt")
// to a full path inside NOTES_DIR. False if it would leave the folder.
bool resolveNotePath(const std::string& in, std::string& out) {
    std::string name = in;
    const std::string prefix = std::string(NOTES_DIR) + "/";
    if (name.compare(0, prefix.size(), prefix) == 0) name.erase(0, prefix.size());
    if (name.empty() || name.size() > NOTE_NAME_MAX || name[0] == '.') return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    if (name.find('.') == std::string::npos) name += ".txt";   // "shopping" -> shopping.txt
    out = prefix + name;
    return true;
}

bool listNote(const sd_storage::DirEntry& entry, void* ctx) {
    auto& notes = *static_cast<JsonArray*>(ctx);
    if (entry.is_dir || entry.name[0] == '.') return true;
    size_t len = strlen(entry.name);
    if (len > 4 && strcmp(entry.name + len - 4, ".tmp") == 0) return true;   // writeAtomic's
    JsonObject note = notes.add<JsonObject>();
    note["name"] = entry.name;
    note["bytes"] = entry.size;
    struct tm tm_local;
    char when[20];
    if (entry.mtime > 0 && localtime_r(&entry.mtime, &tm_local) &&
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm_local)) {
        note["modified"] = when;
    }
    return notes.size() < NOTE_LIST_MAX;
}

void rejectPath(JsonDocument& response_doc) {
    response_doc["status"] = "error";
    response_doc["message"] = "Invalid note name. Use a plain file name like 'shopping.txt' "
                              "(letters, digits, '-', '_', '.'; no folders).";
}

// A delete's first call: the names the model was told to read back, and
// the model's turn count then. The confirmed call must name the same notes
// after at least one finished turn (the question asked, the user answered),
// so the model can't confirm on its own within one reply.
struct PendingDelete {
    std::vector<std::string> paths;
    uint32_t turn = 0;
};
PendingDelete s_pending_delete;

void ensureNotesDir() {
    if (!sd_storage::Fs::mkdirs(NOTES_DIR)) {
        ESP_LOGW(TAG, "Could not create %s", NOTES_DIR);
    }
}

void listNotes(JsonDocument& response_doc) {
    JsonArray notes = response_doc["notes"].to<JsonArray>();
    if (sd_storage::Fs::isDir(NOTES_DIR)) {
        sd_storage::Fs::list(NOTES_DIR, nullptr, true, listNote, &notes);
    }
    response_doc["status"] = "success";
    if (notes.size() >= NOTE_LIST_MAX) response_doc["truncated"] = true;
}

void readNote(const std::string& name, JsonDocument& response_doc) {
    std::string path;
    if (!resolveNotePath(name, path)) return rejectPath(response_doc);
    if (!sd_storage::Fs::isFile(path.c_str())) {
        response_doc["status"] = "error";
        response_doc["message"] = "Note not found";
        return;
    }
    std::string content = sd_storage::Fs::readText(path.c_str());
    if (content.size() > NOTE_READ_MAX) {
        content.resize(NOTE_READ_MAX);
        response_doc["truncated"] = true;
    }
    response_doc["status"] = "success";
    response_doc["content"] = content;
}

void writeNote(const std::string& name, const std::string& content, JsonDocument& response_doc) {
    std::string path;
    if (!resolveNotePath(name, path)) return rejectPath(response_doc);
    ensureNotesDir();
    bool ok = sd_storage::Fs::writeAtomic(path.c_str(), content.c_str());
    response_doc["status"] = ok ? "success" : "error";
    response_doc["message"] = ok ? "Note saved" : "Failed to save note";
}

void appendNote(const std::string& name, const std::string& content, JsonDocument& response_doc) {
    std::string path;
    if (!resolveNotePath(name, path)) return rejectPath(response_doc);
    ensureNotesDir();
    // One entry per line: start a new line unless the note ends with one.
    std::string text = content;
    if (text.empty() || text.back() != '\n') text += '\n';
    auto file = sd_storage::File::open(path.c_str(), sd_storage::Mode::UpdateOrCreate);
    bool ok = false;
    if (file) {
        char last = '\n';
        long size = file.size();
        if (size > 0 && file.seek(size - 1)) file.read(&last, 1);
        if (last != '\n') text.insert(text.begin(), '\n');
        ok = file.seek(0, SEEK_END) && file.writeAll(text.data(), text.size());
    }
    response_doc["status"] = ok ? "success" : "error";
    response_doc["message"] = ok ? "Added to the note" : "Failed to update the note";
}

// Exact names only (resolveNotePath refuses '*' and paths), at most
// MAX_DELETE_NOTES, all checked before any is deleted.
void deleteNotes(const std::vector<std::string>& names, bool confirmed, JsonDocument& response_doc) {
    if (names.empty() || names.size() > MAX_DELETE_NOTES) {
        response_doc["status"] = "error";
        response_doc["message"] = "Give 1 to 10 exact note names (see action list)";
        return;
    }
    std::vector<std::string> paths(names.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        if (!resolveNotePath(names[i], paths[i])) return rejectPath(response_doc);
    }
    // Sorted and unique, so the confirmed call may list them in any order.
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    uint32_t turn = VoiceAgent::active().turnsCompleted();
    if (!confirmed || paths != s_pending_delete.paths || turn == s_pending_delete.turn) {
        s_pending_delete = {paths, turn};
        JsonArray notes = response_doc["notes"].to<JsonArray>();
        int found = 0;
        for (const auto& p : paths) {
            JsonObject n = notes.add<JsonObject>();
            n["name"] = p.substr(strlen(NOTES_DIR) + 1);
            bool exists = sd_storage::Fs::isFile(p.c_str());
            n["exists"] = exists;
            found += exists;
        }
        response_doc["status"] = "confirm";
        response_doc["message"] = found == 0
            ? "None of these notes exist; nothing to delete. Use action list for the names."
            : "Nothing deleted yet. Read the existing names to the user and ask them to confirm. "
              "If they agree, call notes again with action delete, the same names and confirmed=true.";
        return;
    }
    s_pending_delete = {};
    JsonArray results = response_doc["results"].to<JsonArray>();
    int deleted = 0;
    for (const auto& p : paths) {
        JsonObject r = results.add<JsonObject>();
        r["name"] = p.substr(strlen(NOTES_DIR) + 1);
        if (!sd_storage::Fs::isFile(p.c_str())) {
            r["result"] = "not found";
        } else if (sd_storage::Fs::remove(p.c_str())) {
            r["result"] = "deleted";
            ++deleted;
        } else {
            r["result"] = "failed";
        }
    }
    response_doc["status"] = deleted > 0 ? "success" : "error";
    response_doc["message"] = std::to_string(deleted) + " of " + std::to_string(paths.size()) + " deleted";
}


tools::Answer notesTool(const tools::Call& call, JsonDocument& reply, void*) {
    auto args = call.skill->args.notes;
    if (args == nullptr) {
        reply["status"] = "error";
        reply["message"] = "Missing arguments";
        return tools::Answer::Now;
    }
    const std::string& action = args->action;
    if (action == "list") {
        listNotes(reply);
    } else if (action == "read") {
        readNote(args->name, reply);
    } else if (action == "write") {
        writeNote(args->name, args->content, reply);
    } else if (action == "append") {
        appendNote(args->name, args->content, reply);
    } else if (action == "delete") {
        std::vector<std::string> names = args->names;
        if (names.empty() && !args->name.empty()) names.push_back(args->name);
        deleteNotes(names, args->confirmed, reply);
    } else {
        reply["status"] = "error";
        reply["message"] = "action must be list, read, write, append or delete";
    }
    return tools::Answer::Now;
}

tools::Answer ledTool(const tools::Call& call, JsonDocument& reply, void*) {
    auto args = call.skill->args.set_led_strip;
    if (args == nullptr) {
        reply["status"] = "error";
        reply["message"] = "Null LED color arguments";
        return tools::Answer::Now;
    }
    uint8_t r = (uint8_t)args->r;
    uint8_t g = (uint8_t)args->g;
    uint8_t b = (uint8_t)args->b;
    EmbeddedSysDb::getInstance().mutate([r, g, b](SystemState& s) {
        s.led.mode = LedMode::SOLID;
        s.led.color = {r, g, b};
    });
    reply["status"] = "success";
    reply["message"] = "RGB LED solid color updated";
    return tools::Answer::Now;
}

tools::Answer memoryTool(const tools::Call& call, JsonDocument& reply, void*) {
    auto args = call.skill->args.save_to_memory;
    if (args == nullptr) {
        reply["status"] = "error";
        reply["message"] = "Null save to memory arguments";
        return tools::Answer::Now;
    }

    const char* path = "/sdcard/gemini_memory.txt";
    sd_storage::PathInfo info;
    bool too_large = sd_storage::Fs::stat(path, info) && info.size >= 16384;

    if (too_large) {
        reply["status"] = "error";
        reply["message"] = "Memory file is full (limit 16 KB). Please request the user to manage or clear memory.";
        return tools::Answer::Now;
    }

    std::string line = args->text + "\n";
    bool ok = sd_storage::Fs::append(path, line.data(), line.size());
    reply["status"] = ok ? "success" : "error";
    reply["message"] = ok ? "Information successfully saved to long-term memory." : "Failed to write to memory file.";
    return tools::Answer::Now;
}

} // namespace

namespace Services {

void registerDeviceTools(tools::Registry& registry) {
    registry.add({"notes", tools::Kind::Device, notesTool, nullptr});
    registry.add({"set_led_strip", tools::Kind::Device, ledTool, nullptr});
    registry.add({"save_to_memory", tools::Kind::Device, memoryTool, nullptr});
}

} // namespace Services
