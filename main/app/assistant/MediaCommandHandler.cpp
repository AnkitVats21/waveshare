#include "MediaCommandHandler.h"
#include "app/mqtt/MqttService.h"
#include "app/media_player/NexusPlayer.h"
#include "common/AppLogger.h"
#include <ArduinoJson.h>

bool MediaCommandHandler::handle(const GeminiSkills::DecodedSkillCall& skill_call, JsonDocument& response_doc) {
    JsonDocument mqtt_doc;
    const char* cmd_name = nullptr;

    switch (skill_call.type) {
        case GeminiSkills::SkillType::PLAY:
            cmd_name = "play";
            mqtt_doc["query"] = skill_call.args.play->query;
            break;
            
        case GeminiSkills::SkillType::PLAY_NEXT:
            // Device-level skip: player manages queue and demands next from server itself.
            LOGI_SYSTEM("Local PLAY_NEXT command — delegating to NexusPlayer::playNext()");
            NexusPlayer::getInstance().playNext();
            response_doc["status"] = "success";
            return true;
            
        case GeminiSkills::SkillType::PAUSE:
            LOGI_SYSTEM("Local Media PAUSE command received");
            NexusPlayer::getInstance().pause();
            response_doc["status"] = "success";
            return true;
            
        case GeminiSkills::SkillType::RESUME:
            LOGI_SYSTEM("Local Media RESUME command received");
            NexusPlayer::getInstance().resume();
            response_doc["status"] = "success";
            return true;
            
        case GeminiSkills::SkillType::STOP:
            cmd_name = "stop";
            break;
            
        case GeminiSkills::SkillType::NEXT:
            // Device-level skip: player manages queue and demands next from server itself.
            LOGI_SYSTEM("Local NEXT command — delegating to NexusPlayer::playNext()");
            NexusPlayer::getInstance().playNext();
            response_doc["status"] = "success";
            return true;
            
        case GeminiSkills::SkillType::PREVIOUS:
            cmd_name = "previous";
            break;
            
        case GeminiSkills::SkillType::VOLUME:
            cmd_name = "volume";
            mqtt_doc["level"] = skill_call.args.volume->level;
            break;
            
        case GeminiSkills::SkillType::MUTE:
            cmd_name = "mute";
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
