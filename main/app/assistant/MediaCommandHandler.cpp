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
            LOGI_SYSTEM("Media PLAY command received: '%s'", skill_call.args.play->query.c_str());
            struct PlayCtx {
                std::string query;
                bool withCaps;
            };
            auto* ctx = new PlayCtx{skill_call.args.play->query, true};

            auto taskFn = [](void* arg) {
                auto* c = static_cast<PlayCtx*>(arg);
                std::string q = std::move(c->query);
                bool caps = c->withCaps;
                delete c;
                MusicPlaybackService::getInstance().play(q.c_str());
                if (caps) {
                    vTaskDeleteWithCaps(NULL);
                } else {
                    vTaskDelete(NULL);
                }
            };

            BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
                taskFn, "bg_play", ThreadConfig::StackSize::STACK_PLAYER,
                ctx, ThreadConfig::Priority::NORMAL, NULL,
                ThreadConfig::CORE_NETWORK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
            );
            if (ret != pdPASS) {
                LOGW_SYSTEM("xTaskCreatePinnedToCoreWithCaps for bg_play returned %d. Retrying internal memory...", (int)ret);
                ctx->withCaps = false;
                ret = xTaskCreatePinnedToCore(
                    taskFn, "bg_play", ThreadConfig::StackSize::STACK_PLAYER,
                    ctx, ThreadConfig::Priority::NORMAL, NULL,
                    ThreadConfig::CORE_NETWORK
                );
            }
            if (ret != pdPASS) {
                LOGE_SYSTEM("CRITICAL: Failed to spawn bg_play task (ret=%d)!", (int)ret);
                delete ctx;
                response_doc["status"] = "error";
                response_doc["message"] = "Resource exhaustion: failed to create player task";
                return false;
            }
            response_doc["status"] = "success";
            return true;
        }

        case GeminiSkills::SkillType::PLAY_NEXT: {
            LOGI_SYSTEM("Media PLAY_NEXT command received: '%s'", skill_call.args.play_next->query.c_str());
            struct PlayCtx {
                std::string query;
                bool withCaps;
            };
            auto* ctx = new PlayCtx{skill_call.args.play_next->query, true};

            auto taskFn = [](void* arg) {
                auto* c = static_cast<PlayCtx*>(arg);
                std::string q = std::move(c->query);
                bool caps = c->withCaps;
                delete c;
                MusicPlaybackService::getInstance().playNext(q.c_str());
                if (caps) {
                    vTaskDeleteWithCaps(NULL);
                } else {
                    vTaskDelete(NULL);
                }
            };

            BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
                taskFn, "bg_play_next", ThreadConfig::StackSize::STACK_PLAYER,
                ctx, ThreadConfig::Priority::NORMAL, NULL,
                ThreadConfig::CORE_NETWORK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
            );
            if (ret != pdPASS) {
                LOGW_SYSTEM("xTaskCreatePinnedToCoreWithCaps for bg_play_next returned %d. Retrying internal memory...", (int)ret);
                ctx->withCaps = false;
                ret = xTaskCreatePinnedToCore(
                    taskFn, "bg_play_next", ThreadConfig::StackSize::STACK_PLAYER,
                    ctx, ThreadConfig::Priority::NORMAL, NULL,
                    ThreadConfig::CORE_NETWORK
                );
            }
            if (ret != pdPASS) {
                LOGE_SYSTEM("CRITICAL: Failed to spawn bg_play_next task (ret=%d)!", (int)ret);
                delete ctx;
                response_doc["status"] = "error";
                response_doc["message"] = "Resource exhaustion: failed to create player task";
                return false;
            }
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
