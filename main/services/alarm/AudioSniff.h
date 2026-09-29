#pragma once

#include <cstddef>
#include <cstdint>

namespace Services {

// What the start of an audio file is, from its first bytes (a few KB: the
// first Ogg page, or the WebM header up to the track's CodecID). Files in the
// alarm folder (tones, briefing music) must be Opus in Ogg or WebM, the only
// audio the player decodes.
struct AudioSniff {
    bool playable = false;   // Opus in Ogg or WebM
    const char* format = "unknown";   // "ogg-opus", "webm-opus", "ogg-vorbis", "mp3", "wav", ...
};

// Minimum bytes worth sniffing; fewer give "too short".
constexpr size_t AUDIO_SNIFF_MIN = 64;
constexpr size_t AUDIO_SNIFF_BYTES = 4096;

AudioSniff sniffAudio(const uint8_t* data, size_t len);

} // namespace Services
