#include "MpvCommandHandler.h"
#include "app/mqtt/MqttService.h"
#include "common/AppLogger.h"
#include <ArduinoJson.h>

bool MpvCommandHandler::handle(const GeminiSkills::DecodedSkillCall& skill_call, JsonDocument& response_doc) {
    JsonDocument mqtt_doc;
    const char* cmd_name = nullptr;

    switch (skill_call.type) {
        case GeminiSkills::SkillType::PLAY:
            cmd_name = "play";
            mqtt_doc["query"] = skill_call.args.play->query;
            break;
            
        case GeminiSkills::SkillType::QUEUE:
            cmd_name = "queue";
            mqtt_doc["query"] = skill_call.args.queue->query;
            break;
            
        case GeminiSkills::SkillType::PLAY_NEXT:
            cmd_name = "play_next";
            mqtt_doc["query"] = skill_call.args.play_next->query;
            break;
            
        case GeminiSkills::SkillType::PAUSE:
            cmd_name = "pause";
            break;
            
        case GeminiSkills::SkillType::RESUME:
            cmd_name = "resume";
            break;
            
        case GeminiSkills::SkillType::STOP:
            cmd_name = "stop";
            break;
            
        case GeminiSkills::SkillType::NEXT:
            cmd_name = "next";
            break;
            
        case GeminiSkills::SkillType::PREVIOUS:
            cmd_name = "previous";
            break;
            
        case GeminiSkills::SkillType::SEEK:
            cmd_name = "seek";
            mqtt_doc["seconds"] = skill_call.args.seek->seconds;
            break;
            
        case GeminiSkills::SkillType::VOLUME:
            cmd_name = "volume";
            mqtt_doc["level"] = skill_call.args.volume->level;
            break;
            
        case GeminiSkills::SkillType::MUTE:
            cmd_name = "mute";
            break;
            
        case GeminiSkills::SkillType::SHUFFLE:
            cmd_name = "shuffle";
            break;
            
        case GeminiSkills::SkillType::CLEAR:
            cmd_name = "clear";
            break;
            
        case GeminiSkills::SkillType::AUTOPLAY:
            cmd_name = "autoplay";
            mqtt_doc["enabled"] = skill_call.args.autoplay->enabled;
            break;
            
        default:
            return false;
    }

    if (cmd_name) {
        mqtt_doc["cmd"] = cmd_name;
        std::string payload;
        serializeJson(mqtt_doc, payload);
        
        LOGI_SYSTEM("Publishing tool command '%s' to MQTT: %s", cmd_name, payload.c_str());
        MqttService::getInstance().publish("mpv/command", payload.c_str());
        
        response_doc["status"] = "success";
        return true;
    }

    return false;
}
