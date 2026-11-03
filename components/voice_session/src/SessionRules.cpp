#include "voice_session/SessionRules.h"

namespace voice {

AfterReply decide(const SessionFacts& facts, const Rules& rules) {
    AfterReply out;
    out.window_ms = rules.followup_ms;
    out.music = MusicAfter::Hold;
    // A music command was carried out: the person got what they asked for,
    // and the music (maybe on a speaker the mic hears) should play.
    if (facts.music_ok > 0 || facts.hub_music_done ||
        (rules.end_after_failed_music && facts.music_failed > 0)) {
        out.end = true;
    }
    return out;
}

} // namespace voice
