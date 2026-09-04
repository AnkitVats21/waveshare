#include "MediaCommandHandler.h"
#include "app/media_player/MusicPlaybackService.h"
#include "common/AppLogger.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include <ArduinoJson.h>

bool MediaCommandHandler::handle(const GeminiSkills::DecodedSkillCall& skill_call, JsonDocument& response_doc) {
    switch (skill_call.type) {
        case GeminiSkills::SkillType::PLAY: {
            LOGI_SYSTEM("Media PLAY command received: '%s'", skill_call.args.play->query.c_str());
            std::string q = skill_call.args.play->query;
            xTaskCreate([](void* arg) {
                std::string* query_str = static_cast<std::string*>(arg);
                MusicPlaybackService::getInstance().play(query_str->c_str());
                delete query_str;
                vTaskDelete(NULL);
            }, "bg_play", ThreadConfig::StackSize::STACK_PLAYER, new std::string(q), ThreadConfig::Priority::NORMAL, NULL);
            response_doc["status"] = "success";
            return true;
        }

        case GeminiSkills::SkillType::PLAY_NEXT: {
            LOGI_SYSTEM("Media PLAY_NEXT command received: '%s'", skill_call.args.play_next->query.c_str());
            std::string q = skill_call.args.play_next->query;
            xTaskCreate([](void* arg) {
                std::string* query_str = static_cast<std::string*>(arg);
                MusicPlaybackService::getInstance().playNext(query_str->c_str());
                delete query_str;
                vTaskDelete(NULL);
            }, "bg_play_next", ThreadConfig::StackSize::STACK_PLAYER, new std::string(q), ThreadConfig::Priority::NORMAL, NULL);
            response_doc["status"] = "success";
            return true;
        }

        case GeminiSkills::SkillType::PAUSE:
            LOGI_SYSTEM("Media PAUSE command received");
            MusicPlaybackService::getInstance().pause();
            response_doc["status"] = "success";
            return true;

        case GeminiSkills::SkillType::RESUME:
            LOGI_SYSTEM("Media RESUME command received");
            MusicPlaybackService::getInstance().resume();
            response_doc["status"] = "success";
            return true;

        case GeminiSkills::SkillType::STOP:
            LOGI_SYSTEM("Media STOP command received");
            MusicPlaybackService::getInstance().stop();
            response_doc["status"] = "success";
            return true;

        case GeminiSkills::SkillType::NEXT: {
            LOGI_SYSTEM("Media NEXT command received");
            const bool ok = MusicPlaybackService::getInstance().next();
            response_doc["status"] = ok ? "success" : "idle";
            return true;
        }

        case GeminiSkills::SkillType::PREVIOUS: {
            LOGI_SYSTEM("Media PREVIOUS command received");
            const bool ok = MusicPlaybackService::getInstance().previous();
            response_doc["status"] = ok ? "success" : "error";
            return true;
        }

        case GeminiSkills::SkillType::VOLUME: {
            int level = skill_call.args.volume->level;
            LOGI_SYSTEM("Media VOLUME command received: %d", level);
            if (level < 0) level = 0;
            if (level > 100) level = 100;
            EmbeddedSysDb::getInstance().mutate([level](SystemState& s) {
                s.audio.speaker_volume = level;
            });
            response_doc["status"] = "success";
            response_doc["volume"] = level;
            return true;
        }

        case GeminiSkills::SkillType::MUTE: {
            LOGI_SYSTEM("Media MUTE command received");
            EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
                s.audio.speaker_volume = 0;
            });
            response_doc["status"] = "success";
            return true;
        }

        case GeminiSkills::SkillType::AUTOPLAY: {
            bool enabled = skill_call.args.autoplay->enabled;
            LOGI_SYSTEM("Media AUTOPLAY command received: %s", enabled ? "enabled" : "disabled");
            MusicPlaybackService::getInstance().setAutoplay(enabled);
            response_doc["status"] = "success";
            return true;
        }

        case GeminiSkills::SkillType::SET_CACHING: {
            bool enabled = skill_call.args.set_caching->enabled;
            LOGI_SYSTEM("Media SET_CACHING command received: %s", enabled ? "enabled" : "disabled");
            MusicPlaybackService::getInstance().setCaching(enabled);
            response_doc["status"] = "success";
            response_doc["caching"] = enabled;
            return true;
        }

        default:
            break;
    }

    return false;
}
