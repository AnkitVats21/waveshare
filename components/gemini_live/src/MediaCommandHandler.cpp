#include "MediaCommandHandler.h"
#include "app/media_player/MusicPlaybackService.h"
#include "common/AppLogger.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include "freertos/idf_additions.h"
#include <ArduinoJson.h>

bool MediaCommandHandler::handle(const GeminiSkills::DecodedSkillCall& skill_call, JsonDocument& response_doc) {
    switch (skill_call.type) {
        case GeminiSkills::SkillType::PLAY: {
            const auto* args = skill_call.args.play;
            bool next = args->when == "next";
            LOGI_SYSTEM("Media PLAY%s command received: '%s'", next ? " (next)" : "", args->query.c_str());
            MusicPlaybackService::getInstance().postCommand(next ? MediaCmdType::PLAY_NEXT : MediaCmdType::PLAY,
                                                            args->query.c_str());
            response_doc["status"] = "success";
            return true;
        }

        case GeminiSkills::SkillType::PLAYBACK: {
            const std::string& action = skill_call.args.playback->action;
            MediaCmdType cmd;
            if (action == "pause") cmd = MediaCmdType::PAUSE;
            else if (action == "resume") cmd = MediaCmdType::RESUME;
            else if (action == "stop") cmd = MediaCmdType::STOP;
            else if (action == "next") cmd = MediaCmdType::NEXT;
            else if (action == "previous") cmd = MediaCmdType::PREVIOUS;
            else {
                response_doc["status"] = "error";
                response_doc["message"] = "action must be pause, resume, stop, next or previous";
                return true;
            }
            LOGI_SYSTEM("Media playback command received: %s", action.c_str());
            MusicPlaybackService::getInstance().postCommand(cmd);
            response_doc["status"] = "success";
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

        case GeminiSkills::SkillType::MUSIC_SETTINGS: {
            const auto* args = skill_call.args.music_settings;
            if (!args->autoplay.empty()) {
                bool on = args->autoplay == "on";
                LOGI_SYSTEM("Media autoplay %s", on ? "on" : "off");
                MusicPlaybackService::getInstance().setAutoplay(on);
                response_doc["autoplay"] = on;
            }
            if (!args->caching.empty()) {
                bool on = args->caching == "on";
                LOGI_SYSTEM("Media caching %s", on ? "on" : "off");
                MusicPlaybackService::getInstance().setCaching(on);
                response_doc["caching"] = on;
            }
            response_doc["status"] = "success";
            return true;
        }

        case GeminiSkills::SkillType::MUSIC_OUTPUT: {
            auto& music = MusicPlaybackService::getInstance();
            RemoteOutput* remote = music.remoteOutput();
            const std::string& target = skill_call.args.music_output->target;
            if (!remote) {
                response_doc["status"] = "error";
                response_doc["message"] = "this device has no satellite support";
                return true;
            }
            std::string chosen;
            if (!target.empty() && remote->select(target, chosen)) {
                LOGI_SYSTEM("Music output -> %s", chosen.c_str());
                response_doc["status"] = "success";
                response_doc["now_playing_on"] = chosen == "local" ? "board" : chosen;
                return true;
            }
            // No target, or no satellite by that name: say what there is.
            const auto sats = remote->satellites();
            const bool onSatellite = EmbeddedSysDb::getInstance().snapshot().media.output_target ==
                                     MediaOutputTarget::SATELLITE;
            std::string current = "board";
            JsonArray names = response_doc["satellites"].to<JsonArray>();
            for (const auto& s : sats) {
                names.add(s.name);
                if (s.active && onSatellite) current = s.name;
            }
            response_doc["status"] = target.empty() ? "success" : "error";
            if (!target.empty()) {
                response_doc["message"] = sats.empty() ? "no satellite is connected" : "no satellite by that name";
            }
            response_doc["now_playing_on"] = current;
            return true;
        }

        default:
            break;
    }

    return false;
}
