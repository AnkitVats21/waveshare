#pragma once

#include <algorithm>
#include <cstdint>

namespace App {

// Limits that end a session that is stuck. All the tunables are here.
namespace SessionLimits {
// Safety net A: VAD reports speech, but no words arrive from the model's
// input transcription for this long (music or noise nearby, no AEC
// reference when the music plays on another device).
constexpr int64_t SOUND_NO_WORDS_MS = 10000;
// Speech frames more than this far apart are separate sounds, not one run.
constexpr int64_t SPEECH_GAP_RESET_MS = 3000;
// Safety net B: the silence timeout is held while a reply is owed (or a
// media command waits for its reply); this is the longest such a hold lasts.
constexpr int64_t REPLY_HOLD_MAX_MS = 20000;
// How often the speech path evaluates the rules.
constexpr int64_t CHECK_EVERY_MS = 250;
}  // namespace SessionLimits

// Pure rules, times in ms from any monotonic clock; host-tested.
class SessionGuard {
public:
    enum class Verdict { Ok, SoundNoWords, ReplyHoldExpired };

    // A session starts (wake): forget the last one.
    void reset() {
        m_speech_since = m_last_speech = m_hold_since = m_last_check = kNone;
    }

    // One VAD speech frame (cheap; call for every frame).
    void noteSpeech(int64_t now) {
        if (m_speech_since == kNone || now - m_last_speech > SessionLimits::SPEECH_GAP_RESET_MS) {
            m_speech_since = now;
        }
        m_last_speech = now;
    }

    // True when the speech path should evaluate now (rate limit).
    bool dueForCheck(int64_t now) {
        if (m_last_check != kNone && now - m_last_check < SessionLimits::CHECK_EVERY_MS) return false;
        m_last_check = now;
        return true;
    }

    // The reply hold: `holding` is whether something owes a reply. Expired
    // once it has lasted REPLY_HOLD_MAX_MS.
    bool holdExpired(int64_t now, bool holding) {
        if (!holding) {
            m_hold_since = kNone;
            return false;
        }
        if (m_hold_since == kNone) m_hold_since = now;
        return now - m_hold_since >= SessionLimits::REPLY_HOLD_MAX_MS;
    }

    // While speech is being reported. last_words: when the last input
    // transcription text arrived (kNone if none). reply_busy: a reply is
    // owed, arriving or playing, so words are not expected from the person.
    Verdict check(int64_t now, int64_t last_words, bool reply_busy, bool holding) {
        if (holdExpired(now, holding)) return Verdict::ReplyHoldExpired;
        if (reply_busy) {
            m_speech_since = now;   // the run counts from when the reply ended
            return Verdict::Ok;
        }
        if (m_speech_since == kNone) return Verdict::Ok;
        const int64_t since = std::max(m_speech_since, last_words);
        return now - since >= SessionLimits::SOUND_NO_WORDS_MS ? Verdict::SoundNoWords : Verdict::Ok;
    }

    static constexpr int64_t kNone = INT64_MIN / 2;

private:
    int64_t m_speech_since = kNone;
    int64_t m_last_speech = kNone;
    int64_t m_hold_since = kNone;
    int64_t m_last_check = kNone;
};

}  // namespace App
