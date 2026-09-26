#include "DeviceCommandHandler.h"
#include "gemini_skills_generated.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "sd_storage/Fs.h"

static const char* TAG = "DeviceCmd";

namespace {

// write_file/read_file are confined to one flat notes folder so the model
// can't read secrets (gemini_config.json) or overwrite device config.
constexpr const char* NOTES_DIR = "/sdcard/notes";
constexpr size_t NOTE_NAME_MAX = 64;
constexpr size_t NOTE_READ_MAX = 8192;  // keeps the tool response small

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
    out = prefix + name;
    return true;
}

void rejectPath(JsonDocument& response_doc) {
    response_doc["status"] = "error";
    response_doc["message"] = "Invalid note name. Use a plain file name like 'shopping.txt' "
                              "(letters, digits, '-', '_', '.'; no folders).";
}

} // namespace

IDeviceCommandDelegate* DeviceCommandHandler::s_delegate = nullptr;

void DeviceCommandHandler::setDelegate(IDeviceCommandDelegate* delegate) {
    s_delegate = delegate;
}

IDeviceCommandDelegate* DeviceCommandHandler::getDelegate() {
    return s_delegate;
}

bool DeviceCommandHandler::handle(const GeminiSkills::DecodedSkillCall& skill_call, JsonDocument& response_doc) {
    using namespace GeminiSkills;

    switch (skill_call.type) {
        case SkillType::WRITE_FILE: {
            auto args = skill_call.args.write_file;
            if (args == nullptr) {
                response_doc["status"] = "error";
                response_doc["message"] = "Null write file arguments";
                return true;
            }
            std::string path;
            if (!resolveNotePath(args->path, path)) {
                rejectPath(response_doc);
                return true;
            }
            if (!sd_storage::Fs::mkdirs(NOTES_DIR)) {
                ESP_LOGW(TAG, "Could not create %s", NOTES_DIR);
            }
            bool ok = sd_storage::Fs::writeAtomic(path.c_str(), args->content.c_str());
            response_doc["status"] = ok ? "success" : "error";
            response_doc["message"] = ok ? "Note saved" : "Failed to save note";
            return true;
        }
            
        case SkillType::READ_FILE: {
            auto args = skill_call.args.read_file;
            if (args == nullptr) {
                response_doc["status"] = "error";
                response_doc["message"] = "Null read file arguments";
                return true;
            }
            std::string path;
            if (!resolveNotePath(args->path, path)) {
                rejectPath(response_doc);
                return true;
            }
            if (!sd_storage::Fs::isFile(path.c_str())) {
                response_doc["status"] = "error";
                response_doc["message"] = "Note not found";
                return true;
            }
            std::string content = sd_storage::Fs::readText(path.c_str());
            if (content.size() > NOTE_READ_MAX) {
                content.resize(NOTE_READ_MAX);
                response_doc["truncated"] = true;
            }
            response_doc["status"] = "success";
            response_doc["content"] = content;
            return true;
        }
            
        case SkillType::SET_LED_STRIP: {
            auto args = skill_call.args.set_led_strip;
            if (args == nullptr) {
                response_doc["status"] = "error";
                response_doc["message"] = "Null LED color arguments";
                return true;
            }
            uint8_t r = (uint8_t)args->r;
            uint8_t g = (uint8_t)args->g;
            uint8_t b = (uint8_t)args->b;
            EmbeddedSysDb::getInstance().mutate([r, g, b](SystemState& s) {
                s.led.mode = LedMode::SOLID;
                s.led.color = {r, g, b};
            });
            response_doc["status"] = "success";
            response_doc["message"] = "RGB LED solid color updated";
            return true;
        }
            
        case SkillType::SAVE_TO_MEMORY: {
            auto args = skill_call.args.save_to_memory;
            if (args == nullptr) {
                response_doc["status"] = "error";
                response_doc["message"] = "Null save to memory arguments";
                return true;
            }

            const char* path = "/sdcard/gemini_memory.txt";
            sd_storage::PathInfo info;
            bool too_large = sd_storage::Fs::stat(path, info) && info.size >= 16384;

            if (too_large) {
                response_doc["status"] = "error";
                response_doc["message"] = "Memory file is full (limit 16 KB). Please request the user to manage or clear memory.";
                return true;
            }

            std::string line = args->text + "\n";
            bool ok = sd_storage::Fs::append(path, line.data(), line.size());
            response_doc["status"] = ok ? "success" : "error";
            response_doc["message"] = ok ? "Information successfully saved to long-term memory." : "Failed to write to memory file.";
            return true;
        }

        case SkillType::STOP_ACTIVE_ALARM:
        case SkillType::SNOOZE_ALARM:
        case SkillType::SET_ALARM:
        case SkillType::LIST_ALARMS:
        case SkillType::CANCEL_ALARM:
        case SkillType::SET_TIMER:
        case SkillType::SET_REMINDER:
        case SkillType::LIST_REMINDERS:
        case SkillType::CANCEL_REMINDER:
        case SkillType::ACKNOWLEDGE_REMINDERS: {
            if (!s_delegate || !s_delegate->handleAlarmTool(skill_call, response_doc)) {
                response_doc["status"] = "error";
                response_doc["message"] = "Alarm service unavailable";
            }
            return true;
        }

        case SkillType::UNKNOWN: {
            ESP_LOGW(TAG, "Received unrecognized tool call.");
            response_doc["status"] = "error";
            response_doc["message"] = "Tool not supported on this firmware version.";
            return true;
        }

        default:
            // Forward to the Media command handler
            return false;
    }
}
