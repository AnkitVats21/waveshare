#include "services/alarm/AudioSniff.h"

#include <cstring>

namespace Services {
namespace {

bool startsWith(const uint8_t* d, size_t len, size_t at, const char* s, size_t n) {
    return at + n <= len && std::memcmp(d + at, s, n) == 0;
}

bool contains(const uint8_t* d, size_t len, const char* s) {
    const size_t n = std::strlen(s);
    for (size_t i = 0; i + n <= len; ++i) {
        if (std::memcmp(d + i, s, n) == 0) return true;
    }
    return false;
}

AudioSniff result(bool playable, const char* format) {
    AudioSniff r;
    r.playable = playable;
    r.format = format;
    return r;
}

} // namespace

AudioSniff sniffAudio(const uint8_t* d, size_t len) {
    if (!d || len < AUDIO_SNIFF_MIN) return result(false, "too short");

    // Ogg: the first page (27-byte header + segment table) holds the codec's
    // identification packet.
    if (startsWith(d, len, 0, "OggS", 4)) {
        const size_t packet = 27 + size_t(d[26]);
        if (startsWith(d, len, packet, "OpusHead", 8)) return result(true, "ogg-opus");
        if (startsWith(d, len, packet, "\x01vorbis", 7)) return result(false, "ogg-vorbis");
        if (startsWith(d, len, packet, "\x7F" "FLAC", 5)) return result(false, "ogg-flac");
        return result(false, "ogg-other");
    }
    // WebM / Matroska: EBML magic, then the track's CodecID string.
    if (startsWith(d, len, 0, "\x1A\x45\xDF\xA3", 4)) {
        if (contains(d, len, "A_OPUS")) return result(true, "webm-opus");
        if (contains(d, len, "A_VORBIS")) return result(false, "webm-vorbis");
        if (contains(d, len, "A_AAC")) return result(false, "webm-aac");
        return result(false, "webm-other");
    }
    if (startsWith(d, len, 0, "ID3", 3) || (d[0] == 0xFF && (d[1] & 0xE0) == 0xE0)) return result(false, "mp3");
    if (startsWith(d, len, 0, "RIFF", 4) && startsWith(d, len, 8, "WAVE", 4)) return result(false, "wav");
    if (startsWith(d, len, 0, "fLaC", 4)) return result(false, "flac");
    if (startsWith(d, len, 4, "ftyp", 4)) return result(false, "mp4");
    return result(false, "unknown");
}

} // namespace Services
