#include "voice_session/SessionRules.h"
#include <cctype>
#include <cstring>

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

const char* errorKindName(ErrorKind kind) {
    switch (kind) {
        case ErrorKind::None: return "";
        case ErrorKind::Quota: return "quota";
        case ErrorKind::Auth: return "auth";
        case ErrorKind::Model: return "model";
        case ErrorKind::Network: return "network";
        default: return "other";
    }
}

namespace {

// Case-insensitive substring.
bool has(const std::string& text, const char* word) {
    return text.find(word) != std::string::npos;
}

std::string lower(const char* s) {
    std::string out = s ? s : "";
    for (char& c : out) c = char(std::tolower((unsigned char)c));
    return out;
}

std::string bare(const std::string& model) {
    return model.rfind("models/", 0) == 0 ? model.substr(7) : model;
}

} // namespace

ErrorKind classifyError(int code, const char* reason) {
    const std::string r = lower(reason);
    // Gemini closes with 1011 "...quota..." / RESOURCE_EXHAUSTED when the
    // key's quota for the model is used up.
    if (has(r, "quota") || has(r, "resource_exhausted") || has(r, "rate limit") || has(r, "429")) {
        return ErrorKind::Quota;
    }
    if (has(r, "api key") || has(r, "api_key") || has(r, "permission") || has(r, "unauthenticated") ||
        has(r, "no gemini key") || has(r, "401") || has(r, "403")) {
        return ErrorKind::Auth;
    }
    // 1007/1008 about the model: a name that doesn't exist, or a model
    // that doesn't do Live.
    if (has(r, "model") && (has(r, "not found") || has(r, "not supported") || has(r, "is not") ||
                            has(r, "invalid") || has(r, "unsupported"))) {
        return ErrorKind::Model;
    }
    if (code < 0 || code == 1006 || has(r, "timeout") || has(r, "timed out") || has(r, "connection") ||
        has(r, "eof") || has(r, "link") || has(r, "network") || has(r, "dns")) {
        return ErrorKind::Network;
    }
    return ErrorKind::Other;
}

std::string pickModel(const std::string& primary, bool fallback_active, const Rules& rules) {
    if (!fallback_active || rules.fallback_model.empty()) return primary;
    if (bare(primary) == bare(rules.fallback_model)) return primary;   // the fallback failed too
    return rules.fallback_model;
}

} // namespace voice
