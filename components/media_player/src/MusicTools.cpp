#include "media_player/MusicTools.h"
#include "media_player/MusicPlaybackService.h"
#include "core_sysdb/AppLogger.h"
#include "core_sysdb/EmbeddedSysDb.h"
#include <ArduinoJson.h>

// The music tools the voice model can call. In hub mode the hub declares its
// own music tools under the same names, and those replace these.

namespace {

using tools::Answer;
using tools::Call;

Answer playTool(const Call& call, JsonDocument& reply, void*) {
    const auto* args = call.skill->args.play;
    bool next = args->when == "next";
    LOGI_SYSTEM("Media PLAY%s command received: '%s'", next ? " (next)" : "", args->query.c_str());
    MusicPlaybackService::getInstance().postCommand(next ? MediaCmdType::PLAY_NEXT : MediaCmdType::PLAY,
                                                    args->query.c_str());
    reply["status"] = "success";
    return Answer::Now;
}

Answer playbackTool(const Call& call, JsonDocument& reply, void*) {
    const std::string& action = call.skill->args.playback->action;
    MediaCmdType cmd;
    if (action == "pause") cmd = MediaCmdType::PAUSE;
    else if (action == "resume") cmd = MediaCmdType::RESUME;
    else if (action == "stop") cmd = MediaCmdType::STOP;
    else if (action == "next") cmd = MediaCmdType::NEXT;
    else if (action == "previous") cmd = MediaCmdType::PREVIOUS;
    else {
        reply["status"] = "error";
        reply["message"] = "action must be pause, resume, stop, next or previous";
        return Answer::Now;
    }
    LOGI_SYSTEM("Media playback command received: %s", action.c_str());
    MusicPlaybackService::getInstance().postCommand(cmd);
    reply["status"] = "success";
    return Answer::Now;
}

Answer volumeTool(const Call& call, JsonDocument& reply, void*) {
    int level = call.skill->args.volume->level;
    LOGI_SYSTEM("Media VOLUME command received: %d", level);
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    EmbeddedSysDb::getInstance().mutate([level](SystemState& s) {
        s.audio.speaker_volume = level;
    });
    reply["status"] = "success";
    reply["volume"] = level;
    return Answer::Now;
}

Answer musicSettingsTool(const Call& call, JsonDocument& reply, void*) {
    const auto* args = call.skill->args.music_settings;
    if (!args->autoplay.empty()) {
        bool on = args->autoplay == "on";
        LOGI_SYSTEM("Media autoplay %s", on ? "on" : "off");
        MusicPlaybackService::getInstance().setAutoplay(on);
        reply["autoplay"] = on;
    }
    if (!args->caching.empty()) {
        bool on = args->caching == "on";
        LOGI_SYSTEM("Media caching %s", on ? "on" : "off");
        MusicPlaybackService::getInstance().setCaching(on);
        reply["caching"] = on;
    }
    reply["status"] = "success";
    return Answer::Now;
}

Answer musicOutputTool(const Call& call, JsonDocument& reply, void*) {
    auto& music = MusicPlaybackService::getInstance();
    RemoteOutput* remote = music.remoteOutput();
    const std::string& target = call.skill->args.music_output->target;
    if (music.hubActive()) {
        // The hub picks the speaker (a direct session over hub music).
        if (target.empty()) {
            // Say what there is, from the hub's last music_state.
            const MusicHub::Speakers sp = music.hubSpeakers();
            JsonArray names = reply["satellites"].to<JsonArray>();
            for (const auto& n : sp.names) names.add(n);
            reply["status"] = "success";
            reply["now_playing_on"] = sp.here ? std::string("board")
                                           : sp.playing_on.empty() ? std::string("nothing") : sp.playing_on;
            return Answer::Now;
        }
        bool sent = music.hubOutput(target == "board" ? "here" : target.c_str());
        LOGI_SYSTEM("Music output -> %s (asked the hub)", target.c_str());
        reply["status"] = sent ? "success" : "error";
        if (!sent) reply["message"] = "the hub did not get it";
        return Answer::Now;
    }
    if (!remote) {
        reply["status"] = "error";
        reply["message"] = "this device has no satellite support";
        return Answer::Now;
    }
    std::string chosen;
    if (!target.empty() && remote->select(target, chosen)) {
        LOGI_SYSTEM("Music output -> %s", chosen.c_str());
        reply["status"] = "success";
        reply["now_playing_on"] = chosen == "local" ? "board" : chosen;
        return Answer::Now;
    }
    // No target, or no satellite by that name: say what there is.
    const auto sats = remote->satellites();
    const bool onSatellite = EmbeddedSysDb::getInstance().snapshot().media.output_target ==
                             MediaOutputTarget::SATELLITE;
    std::string current = "board";
    JsonArray names = reply["satellites"].to<JsonArray>();
    for (const auto& s : sats) {
        names.add(s.name);
        if (s.active && onSatellite) current = s.name;
    }
    reply["status"] = target.empty() ? "success" : "error";
    if (!target.empty()) {
        reply["message"] = sats.empty() ? "no satellite is connected" : "no satellite by that name";
    }
    reply["now_playing_on"] = current;
    return Answer::Now;
}

} // namespace

void registerMusicTools(tools::Registry& registry) {
    registry.add({"play", tools::Kind::Music, playTool, nullptr});
    registry.add({"playback", tools::Kind::Music, playbackTool, nullptr});
    registry.add({"volume", tools::Kind::Music, volumeTool, nullptr});
    registry.add({"music_settings", tools::Kind::Music, musicSettingsTool, nullptr});
    registry.add({"music_output", tools::Kind::Music, musicOutputTool, nullptr});
}
