#include "DeviceCommandHandler.h"
#include "gemini_skills_generated.h"
#include "services/storage/StorageService.h"
#include "services/alarm/AlarmService.h"
#include "app/mqtt/MqttService.h"
#include "common/sysdb/EmbeddedSysDb.h"
// #include "common/AppLogger.h"
#include "esp_log.h"
// #include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "DeviceCmd";

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
            bool ok = Services::StorageService::getInstance().writeFile(args->path.c_str(), args->content.c_str());
            response_doc["status"] = ok ? "success" : "error";
            response_doc["message"] = ok ? "File written successfully" : "Failed to write file";
            return true;
        }
            
        case SkillType::READ_FILE: {
            auto args = skill_call.args.read_file;
            if (args == nullptr) {
                response_doc["status"] = "error";
                response_doc["message"] = "Null read file arguments";
                return true;
            }
            std::string content = Services::StorageService::getInstance().readFile(args->path.c_str());
            if (!content.empty() || Services::StorageService::getInstance().fileExists(args->path.c_str())) {
                response_doc["status"] = "success";
                response_doc["content"] = content;
            } else {
                response_doc["status"] = "error";
                response_doc["message"] = "File not found or empty";
            }
            return true;
        }
            
        case SkillType::RESTART_WEBSOCKET_CLIENT: {
            ESP_LOGI(TAG, "Tool request: Restarting WebSocket client...");
            EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                s.assistant.connect_requested = false;
            });
            // Yield task execution briefly to allow the connection to tear down
            vTaskDelay(pdMS_TO_TICKS(1000));
            EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                s.assistant.connect_requested = true;
            });
            response_doc["status"] = "success";
            response_doc["message"] = "WebSocket client restart triggered";
            return true;
        }
            
        case SkillType::SET_DEVICE_VOLUME: {
            auto args = skill_call.args.set_device_volume;
            if (args == nullptr) {
                response_doc["status"] = "error";
                response_doc["message"] = "Null volume level arguments";
                return true;
            }
            int vol = (int)args->level;
            if (vol < 0) vol = 0;
            if (vol > 100) vol = 100;
            EmbeddedSysDb::getInstance().mutate([vol](SystemState& s) {
                s.audio.speaker_volume = vol;
            });
            response_doc["status"] = "success";
            response_doc["message"] = "Volume level adjusted successfully";
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
            
        case SkillType::MQTT_FORWARD: {
            auto args = skill_call.args.mqtt_forward;
            if (args == nullptr) {
                response_doc["status"] = "error";
                response_doc["message"] = "Null MQTT forwarding arguments";
                return true;
            }
            bool ok = MqttService::getInstance().publish(args->topic.c_str(), args->message.c_str());
            response_doc["status"] = ok ? "success" : "error";
            response_doc["message"] = ok ? "MQTT forward message published" : "Failed to publish forward message";
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

            // Find if an alarm already exists at this time or find next free ID
            auto alarms = Services::AlarmService::getInstance().getAlarms();
            int target_id = -1;
            int max_id = 0;
            for (const auto& a : alarms) {
                if (a.id > max_id) {
                    max_id = a.id;
                }
                if (a.hour == hour && a.minute == minute) {
                    target_id = a.id;
                }
            }

            if (target_id == -1) {
                target_id = max_id + 1;
            }

            Services::Alarm alarm;
            alarm.id = target_id;
            alarm.hour = hour;
            alarm.minute = minute;
            alarm.enabled = args->enabled;

            std::string tone = args->tone_file;
            if (tone.empty()) {
                tone = "/sdcard/alarms/soft_wake_up.wav";
            }
            strncpy(alarm.tone_file, tone.c_str(), sizeof(alarm.tone_file) - 1);
            alarm.tone_file[sizeof(alarm.tone_file) - 1] = '\0';

            ESP_LOGI(TAG, "Tool request: Setting alarm %d for %02d:%02d (%s, %s)...", 
                     alarm.id, alarm.hour, alarm.minute, alarm.tone_file, alarm.enabled ? "enabled" : "disabled");
            
            Services::AlarmService::getInstance().addOrUpdateAlarm(alarm);

            response_doc["status"] = "success";
            response_doc["message"] = "Alarm set successfully";
            response_doc["alarm_id"] = alarm.id;
            return true;
        }

        case SkillType::SAVE_TO_MEMORY: {
            auto args = skill_call.args.save_to_memory;
            if (args == nullptr) {
                response_doc["status"] = "error";
                response_doc["message"] = "Null save to memory arguments";
                return true;
            }
            std::string path = "/sdcard/gemini_memory.txt";
            bool too_large = false;
            if (Services::StorageService::getInstance().fileExists(path.c_str())) {
                std::string current = Services::StorageService::getInstance().readFile(path.c_str());
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
            bool ok = Services::StorageService::getInstance().appendFile(path.c_str(), line.c_str());
            response_doc["status"] = ok ? "success" : "error";
            response_doc["message"] = ok ? "Information successfully saved to long-term memory." : "Failed to write to memory file.";
            return true;
        }

        case SkillType::STOP_ACTIVE_ALARM: {
            ESP_LOGI(TAG, "Tool request: Stopping active alarm tone...");
            Services::AlarmService::getInstance().stopActiveAlarm();
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

        // case SkillType::LIST_ALARMS: {
        //     auto alarms = Services::AlarmService::getInstance().getAlarms();
        //     response_doc["status"] = "success";
        //     response_doc["message"] = "Alarms listed successfully";
        //     response_doc["alarms"] = alarms;
        //     return true;
        // }

        default:
            // Forward to the MPV media command handler
            return false;
    }
}
