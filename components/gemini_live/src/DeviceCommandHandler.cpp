#include "DeviceCommandHandler.h"
#include "gemini_skills_generated.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <cerrno>
#include <sys/stat.h>

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
            if (!s_delegate) {
                response_doc["status"] = "error";
                response_doc["message"] = "Storage service unavailable";
                return true;
            }
            std::string path;
            if (!resolveNotePath(args->path, path)) {
                rejectPath(response_doc);
                return true;
            }
            if (mkdir(NOTES_DIR, 0775) != 0 && errno != EEXIST) {
                ESP_LOGW(TAG, "Could not create %s (errno %d)", NOTES_DIR, errno);
            }
            bool ok = s_delegate->writeFile(path.c_str(), args->content.c_str());
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
            if (!s_delegate) {
                response_doc["status"] = "error";
                response_doc["message"] = "Storage service unavailable";
                return true;
            }
            std::string path;
            if (!resolveNotePath(args->path, path)) {
                rejectPath(response_doc);
                return true;
            }
            if (!s_delegate->fileExists(path.c_str())) {
                response_doc["status"] = "error";
                response_doc["message"] = "Note not found";
                return true;
            }
            std::string content = s_delegate->readFile(path.c_str());
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
            
        case SkillType::SET_ALARM: {
            auto args = skill_call.args.set_alarm;
            if (args == nullptr) {
                response_doc["status"] = "error";
                response_doc["message"] = "Null set alarm arguments";
                return true;
            }

            int hour = args->hour;
            int minute = args->minute;
            if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
                response_doc["status"] = "error";
                response_doc["message"] = "Invalid time format (hour 0-23, minute 0-59 required)";
                return true;
            }

            if (!s_delegate) {
                response_doc["status"] = "error";
                response_doc["message"] = "Alarm service unavailable";
                return true;
            }

            int alarm_id = -1;
            bool ok = s_delegate->setAlarm(hour, minute, args->tone_file.c_str(), args->enabled, alarm_id);
            if (ok) {
                response_doc["status"] = "success";
                response_doc["message"] = "Alarm set successfully";
                response_doc["alarm_id"] = alarm_id;
            } else {
                response_doc["status"] = "error";
                response_doc["message"] = "Failed to set alarm";
            }
            return true;
        }

        case SkillType::SAVE_TO_MEMORY: {
            auto args = skill_call.args.save_to_memory;
            if (args == nullptr) {
                response_doc["status"] = "error";
                response_doc["message"] = "Null save to memory arguments";
                return true;
            }
            if (!s_delegate) {
                response_doc["status"] = "error";
                response_doc["message"] = "Storage service unavailable";
                return true;
            }

            std::string path = "/sdcard/gemini_memory.txt";
            bool too_large = false;
            if (s_delegate->fileExists(path.c_str())) {
                std::string current = s_delegate->readFile(path.c_str());
                if (current.length() >= 16384) {
                    too_large = true;
                }
            }

            if (too_large) {
                response_doc["status"] = "error";
                response_doc["message"] = "Memory file is full (limit 16 KB). Please request the user to manage or clear memory.";
                return true;
            }

            std::string line = args->text + "\n";
            bool ok = s_delegate->appendFile(path.c_str(), line.c_str());
            response_doc["status"] = ok ? "success" : "error";
            response_doc["message"] = ok ? "Information successfully saved to long-term memory." : "Failed to write to memory file.";
            return true;
        }

        case SkillType::STOP_ACTIVE_ALARM: {
            ESP_LOGI(TAG, "Tool request: Stopping active alarm tone...");
            if (!s_delegate) {
                response_doc["status"] = "error";
                response_doc["message"] = "Alarm service unavailable";
                return true;
            }
            s_delegate->stopActiveAlarm();
            response_doc["status"] = "success";
            response_doc["message"] = "Alarm cancellation triggered";
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
