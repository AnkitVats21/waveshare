#include "app/audio/recording/RecordingProbe.h"

#include <cstring>

namespace RecordingProbe {
namespace {

uint32_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint64_t le64(const uint8_t* p) { return le32(p) | (uint64_t(le32(p + 4)) << 32); }

constexpr size_t OGG_HEADER = 27;

// Length of the whole page at `p` if its header, lacing and body fit in
// `avail` bytes and its CRC matches; 0 otherwise.
size_t wholePage(const uint8_t* p, size_t avail) {
    if (avail < OGG_HEADER || memcmp(p, "OggS", 4) != 0 || p[4] != 0) return 0;
    size_t nseg = p[26];
    if (avail < OGG_HEADER + nseg) return 0;
    size_t body = 0;
    for (size_t i = 0; i < nseg; ++i) body += p[OGG_HEADER + i];
    size_t len = OGG_HEADER + nseg + body;
    if (avail < len) return 0;
    static const uint8_t zero[4] = {};
    uint32_t crc = oggCrc(p, 22);
    crc = oggCrc(zero, 4, crc);
    crc = oggCrc(p + 26, len - 26, crc);
    return crc == le32(p + 22) ? len : 0;
}

}  // namespace

uint32_t oggCrc(const uint8_t* data, size_t len, uint32_t crc) {
    for (size_t i = 0; i < len; ++i) {
        crc ^= uint32_t(data[i]) << 24;
        for (int k = 0; k < 8; ++k) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

bool probeOpus(const uint8_t* head, size_t head_len, const uint8_t* tail, size_t tail_len, Info& out) {
    // The first page holds only OpusHead (RFC 7845 section 3).
    if (head_len < OGG_HEADER + 1) return false;
    size_t nseg = head[26];
    size_t at = OGG_HEADER + nseg;
    if (memcmp(head, "OggS", 4) != 0 || head_len < at + 19 || memcmp(head + at, "OpusHead", 8) != 0) return false;
    const uint8_t* oh = head + at;
    uint8_t channels = oh[9];
    uint32_t pre_skip = le16(oh + 10);
    uint32_t input_rate = le32(oh + 12);

    // The last whole page with a granule (-1 means no packet ends on it).
    for (size_t i = tail_len >= OGG_HEADER ? tail_len - OGG_HEADER + 1 : 0; i-- > 0;) {
        if (tail[i] != 'O' || !wholePage(tail + i, tail_len - i)) continue;
        uint64_t granule = le64(tail + i + 6);
        if (granule == ~0ull) continue;
        out.channels = channels;
        out.sample_rate = input_rate;
        out.duration_ms = granule > pre_skip ? uint32_t((granule - pre_skip) * 1000 / 48000) : 0;
        return true;
    }
    return false;
}

bool probeWav(const uint8_t* head, size_t head_len, uint64_t file_size, Info& out) {
    if (head_len < 12 || memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0) return false;
    uint32_t byte_rate = 0;
    Info info;
    for (size_t p = 12; p + 8 <= head_len;) {
        uint32_t size = le32(head + p + 4);
        if (memcmp(head + p, "fmt ", 4) == 0) {
            if (p + 8 + 16 > head_len) return false;
            info.channels = uint8_t(le16(head + p + 10));
            info.sample_rate = le32(head + p + 12);
            byte_rate = le32(head + p + 16);
        } else if (memcmp(head + p, "data", 4) == 0) {
            if (byte_rate == 0) return false;
            uint64_t start = p + 8;
            uint64_t present = file_size > start ? file_size - start : 0;
            uint64_t data = (size == 0 || size > present) ? present : size;
            info.duration_ms = uint32_t(data * 1000 / byte_rate);
            out = info;
            return true;
        }
        if (size >= head_len) return false;  // chunks before "data" are small
        p += 8 + size + (size & 1);
    }
    return false;
}

}  // namespace RecordingProbe
