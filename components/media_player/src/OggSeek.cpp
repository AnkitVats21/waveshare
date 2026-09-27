#include "media_player/OggSeek.h"

#include <cstring>

namespace Media {

namespace {

constexpr size_t HEADER = 27;

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint64_t le64(const uint8_t* p) { return le32(p) | (uint64_t(le32(p + 4)) << 32); }

void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

uint32_t pageCrc(const uint8_t* p, size_t len) {
    static const uint8_t zero[4] = {};
    uint32_t crc = oggCrc(p, 22);
    crc = oggCrc(zero, 4, crc);
    return oggCrc(p + 26, len - 26, crc);
}

} // namespace

uint32_t opusPacketFrames(const uint8_t* pkt, size_t len) {
    if (len == 0) return 0;
    static const uint16_t silk[4] = {480, 960, 1920, 2880};
    static const uint16_t celt[4] = {120, 240, 480, 960};
    const uint8_t config = pkt[0] >> 3;
    const uint32_t size = config < 12 ? silk[config & 3] : config < 16 ? (config & 1 ? 960 : 480) : celt[config & 3];
    switch (pkt[0] & 3) {
    case 0: return size;
    case 1:
    case 2: return 2 * size;
    default: return len < 2 ? 0 : size * (pkt[1] & 0x3F);
    }
}

int64_t oggPageStartGranule(const uint8_t* p, const OggPage& page) {
    // The last page's granule may cut its final packet short (end trimming).
    if (page.granule < 0 || (page.flags & (OGG_CONTINUED | OGG_EOS))) return -1;
    const size_t nseg = p[26];
    const uint8_t* lacing = p + HEADER;
    const uint8_t* body = lacing + nseg;
    int64_t frames = 0;
    size_t start = 0, len = 0;
    for (size_t i = 0; i < nseg; ++i) {
        len += lacing[i];
        if (lacing[i] < 255) {   // a packet ends here
            frames += opusPacketFrames(body + start, len);
            start += len;
            len = 0;
        }
    }
    return page.granule - frames;
}

uint32_t oggCrc(const uint8_t* data, size_t len, uint32_t crc) {
    for (size_t i = 0; i < len; ++i) {
        crc ^= uint32_t(data[i]) << 24;
        for (int k = 0; k < 8; ++k) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

OggPageRead readOggPage(const uint8_t* p, size_t avail, OggPage& page) {
    // Check what is there before asking for more.
    static const char magic[] = "OggS";
    for (size_t i = 0; i < 5 && i < avail; ++i) {
        if (p[i] != (i < 4 ? uint8_t(magic[i]) : 0)) return OggPageRead::NotPage;
    }
    if (avail < HEADER) return OggPageRead::NeedMore;
    const size_t nseg = p[26];
    if (avail < HEADER + nseg) return OggPageRead::NeedMore;
    size_t body = 0;
    for (size_t i = 0; i < nseg; ++i) body += p[HEADER + i];
    const size_t len = HEADER + nseg + body;
    if (avail < len) return OggPageRead::NeedMore;
    if (pageCrc(p, len) != le32(p + 22)) return OggPageRead::NotPage;
    page.flags = p[5];
    page.granule = int64_t(le64(p + 6));
    page.serial = le32(p + 14);
    page.seq = le32(p + 18);
    page.len = len;
    return OggPageRead::Ok;
}

bool parseOggHeader(const uint8_t* data, size_t len, OggHeader& out) {
    OggPage page;
    size_t p = 0;
    uint32_t pages = 0;
    while (true) {
        if (readOggPage(data + p, len - p, page) != OggPageRead::Ok) return false;
        if (pages == 0) {
            // OpusHead alone on the first page (RFC 7845 section 3).
            const uint8_t* oh = data + HEADER + data[26];
            if (!(page.flags & 0x02) || page.len < HEADER + data[26] + 19 || memcmp(oh, "OpusHead", 8) != 0) {
                return false;
            }
            out.serial = page.serial;
            out.pre_skip = uint16_t(oh[10] | (oh[11] << 8));
        } else if (page.serial != out.serial) {
            return false;
        } else if (page.granule > 0 && pages >= 2) {
            break;   // the first audio page
        }
        ++pages;
        p += page.len;
    }
    out.bytes.assign(data, data + p);
    out.pages = pages;
    return true;
}

std::vector<uint8_t> renumberHeader(const OggHeader& header, uint32_t first_audio_seq) {
    if (first_audio_seq < header.pages) return {};
    std::vector<uint8_t> out = header.bytes;
    uint32_t seq = first_audio_seq - header.pages;
    OggPage page;
    for (size_t p = 0; p < out.size(); p += page.len) {
        if (readOggPage(out.data() + p, out.size() - p, page) != OggPageRead::Ok) return {};
        put32(out.data() + p + 18, seq++);
        put32(out.data() + p + 22, pageCrc(out.data() + p, page.len));
    }
    return out;
}

OggSeekScanner::OggSeekScanner(uint32_t serial, uint16_t pre_skip, uint32_t target_ms)
    : _serial(serial), _preSkip(pre_skip), _target(int64_t(target_ms) * 48) {}

size_t OggSeekScanner::scan(const uint8_t* buf, size_t len) {
    size_t p = 0;
    while (!_found && p < len) {
        OggPage page;
        const OggPageRead r = readOggPage(buf + p, len - p, page);
        if (r == OggPageRead::NeedMore) break;
        if (r == OggPageRead::NotPage || page.serial != _serial) {
            // Mid-page (where the estimate landed) or a false match inside one.
            ++p;
            _prevEnd = -1;
            continue;
        }
        // Keep the first page that ends at or after the target and starts a
        // packet; the pages before it are dropped without decoding.
        if (page.granule < 0 || page.granule - _preSkip < _target || (page.flags & OGG_CONTINUED)) {
            _prevEnd = page.granule;
            p += page.len;
            continue;
        }
        _found = true;
        _startSeq = page.seq;
        // The decoder drops pre-skip frames from its first output, which then
        // starts at time start_granule (RFC 7845 section 4.2): where the page
        // before ended, else counted back from this page's end.
        const int64_t start_granule = _prevEnd >= 0 ? _prevEnd : oggPageStartGranule(buf + p, page);
        if (start_granule >= 0 && _target > start_granule) _discard = uint32_t(_target - start_granule);
    }
    return p;
}

} // namespace Media
