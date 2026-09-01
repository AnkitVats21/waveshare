#include "MediaCommandHandler.h"
#include "app/media_player/MusicPlaybackService.h"
#include "app/media_player/NexusPlayer.h"
#include "app/mqtt/MqttService.h"
#include "common/AppLogger.h"
#include <ArduinoJson.h>

bool MediaCommandHandler::handle(const GeminiSkills::DecodedSkillCall& skill_call, JsonDocument& response_doc) {
    // Media playback is now resolved locally through Invidious. MQTT remains
    // available for commands that are still intentionally delegated elsewhere.
    switch (skill_call.type) {
        case GeminiSkills::SkillType::PLAY: {
            const bool ok = MusicPlaybackService::getInstance().play(skill_call.args.play->query);
            response_doc["status"] = ok ? "success" : "error";
            return true;
        }

        case GeminiSkills::SkillType::PLAY_NEXT: {
            const bool ok = MusicPlaybackService::getInstance().playNext(skill_call.args.play_next->query);
            response_doc["status"] = ok ? "success" : "error";
            return true;
        }

        case GeminiSkills::SkillType::PAUSE:
            LOGI_SYSTEM("Local Media PAUSE command received");
            MusicPlaybackService::getInstance().pause();
            response_doc["status"] = "success";
            return true;

        case GeminiSkills::SkillType::RESUME:
            LOGI_SYSTEM("Local Media RESUME command received");
            MusicPlaybackService::getInstance().resume();
            response_doc["status"] = "success";
            return true;

        case GeminiSkills::SkillType::STOP:
            LOGI_SYSTEM("Local Media STOP command received");
            MusicPlaybackService::getInstance().stop();
            response_doc["status"] = "success";
            return true;

        default:
            break;
    }

    JsonDocument mqtt_doc;
    const char* cmd_name = nullptr;

    switch (skill_call.type) {
        case GeminiSkills::SkillType::NEXT:
            cmd_name = "next";
            break;

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

    mqtt_doc["cmd"] = cmd_name;
    std::string payload;
    serializeJson(mqtt_doc, payload);

    LOGI_SYSTEM("Publishing delegated tool command '%s' to MQTT: %s", cmd_name, payload.c_str());
    MqttService::getInstance().publish("mpv/command", payload.c_str());

    response_doc["status"] = "success";
    return true;
}
