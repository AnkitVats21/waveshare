#include "media_player/WebmSeek.h"

#include <algorithm>
#include <cstring>

namespace Media {

namespace {

constexpr uint32_t ID_EBML = 0x1A45DFA3;
constexpr uint32_t ID_SEGMENT = 0x18538067;
constexpr uint32_t ID_INFO = 0x1549A966;
constexpr uint32_t ID_TIMECODE_SCALE = 0x2AD7B1;
constexpr uint32_t ID_DURATION = 0x4489;
constexpr uint32_t ID_CUES = 0x1C53BB6B;
constexpr uint32_t ID_CUE_POINT = 0xBB;
constexpr uint32_t ID_CUE_TIME = 0xB3;
constexpr uint32_t ID_CUE_TRACK_POSITIONS = 0xB7;
constexpr uint32_t ID_CUE_CLUSTER_POSITION = 0xF1;
constexpr uint32_t ID_CLUSTER = 0x1F43B675;
constexpr uint8_t ID_CLUSTER_TIMECODE = 0xE7;

constexpr uint64_t UNKNOWN_SIZE = UINT64_MAX;

// One element header: ID, payload start and payload size.
struct Element {
    uint32_t id;
    size_t data;
    uint64_t size;   // UNKNOWN_SIZE for "until the parent ends"
};

enum class Read : uint8_t { Ok, Short, Bad };

// Reads the element header at `p`, within [p, end).
Read readElement(const uint8_t* b, size_t p, size_t end, Element& out) {
    if (p >= end) return Read::Short;
    // ID: 1-4 bytes, marker bit kept.
    size_t idLen = 1;
    while (idLen <= 4 && !(b[p] & (0x80 >> (idLen - 1)))) ++idLen;
    if (idLen > 4) return Read::Bad;
    if (p + idLen >= end) return Read::Short;
    uint32_t id = 0;
    for (size_t i = 0; i < idLen; ++i) id = (id << 8) | b[p + i];

    // Size: 1-8 bytes, marker bit dropped; all ones means unknown.
    size_t q = p + idLen;
    size_t szLen = 1;
    while (szLen <= 8 && !(b[q] & (0x80 >> (szLen - 1)))) ++szLen;
    if (szLen > 8) return Read::Bad;
    if (q + szLen > end) return Read::Short;
    uint64_t size = b[q] & (0xFF >> szLen);
    for (size_t i = 1; i < szLen; ++i) size = (size << 8) | b[q + i];
    const uint64_t allOnes = (uint64_t(1) << (7 * szLen)) - 1;

    out.id = id;
    out.data = q + szLen;
    out.size = size == allOnes ? UNKNOWN_SIZE : size;
    return Read::Ok;
}

bool readUint(const uint8_t* b, const Element& e, uint64_t& out) {
    if (e.size == 0 || e.size > 8) return false;
    out = 0;
    for (size_t i = 0; i < e.size; ++i) out = (out << 8) | b[e.data + i];
    return true;
}

// A 4- or 8-byte big-endian IEEE float.
bool readFloat(const uint8_t* b, const Element& e, double& out) {
    if (e.size == 4) {
        uint32_t bits = 0;
        for (size_t i = 0; i < 4; ++i) bits = (bits << 8) | b[e.data + i];
        float f;
        memcpy(&f, &bits, sizeof(f));
        out = f;
        return true;
    }
    if (e.size == 8) {
        uint64_t bits = 0;
        for (size_t i = 0; i < 8; ++i) bits = (bits << 8) | b[e.data + i];
        memcpy(&out, &bits, sizeof(out));
        return true;
    }
    return false;
}

// Walks the children of [p, end), calling fn(child) for each; fn returns false
// to fail. Every child must fit and have a known size.
template <typename Fn>
bool forEachChild(const uint8_t* b, size_t p, size_t end, Fn fn) {
    while (p < end) {
        Element e;
        if (readElement(b, p, end, e) != Read::Ok) return false;
        if (e.size == UNKNOWN_SIZE || e.size > end - e.data) return false;
        if (!fn(e)) return false;
        p = e.data + size_t(e.size);
    }
    return true;
}

// Cue times stay in timecode units until the scale is known.
struct RawCue {
    uint64_t time;
    uint64_t position;   // from the start of the Segment's payload
};

bool parseCues(const uint8_t* b, const Element& cues, std::vector<RawCue>& out) {
    return forEachChild(b, cues.data, cues.data + size_t(cues.size), [&](const Element& point) {
        if (point.id != ID_CUE_POINT) return true;
        bool hasTime = false, hasPos = false;
        RawCue cue{};
        bool ok = forEachChild(b, point.data, point.data + size_t(point.size), [&](const Element& f) {
            if (f.id == ID_CUE_TIME) {
                hasTime = readUint(b, f, cue.time);
                return hasTime;
            }
            if (f.id == ID_CUE_TRACK_POSITIONS && !hasPos) {   // the first track's position
                return forEachChild(b, f.data, f.data + size_t(f.size), [&](const Element& t) {
                    if (t.id != ID_CUE_CLUSTER_POSITION) return true;
                    hasPos = readUint(b, t, cue.position);
                    return hasPos;
                });
            }
            return true;
        });
        if (!ok || !hasTime || !hasPos) return false;
        if (out.size() >= MAX_CUES) return false;
        out.push_back(cue);
        return true;
    });
}

WebmCues status(CuesStatus s, uint32_t need = 0) {
    WebmCues r;
    r.status = s;
    r.need = need;
    return r;
}

// NeedMore for a prefix that must reach `end`, or Invalid if that is too far.
WebmCues needUpTo(uint64_t end) {
    return end > MAX_HEADER_BYTES ? status(CuesStatus::Invalid) : status(CuesStatus::NeedMore, uint32_t(end));
}

} // namespace

WebmCues parseWebmCues(const uint8_t* data, size_t len) {
    constexpr size_t MAX_HEADER = 12;   // 4-byte ID + 8-byte size

    Element ebml;
    Read r = readElement(data, 0, len, ebml);
    if (r == Read::Short) return needUpTo(MAX_HEADER);
    if (r == Read::Bad || ebml.id != ID_EBML || ebml.size == UNKNOWN_SIZE) return status(CuesStatus::Invalid);

    const uint64_t segAt = ebml.data + ebml.size;
    Element seg;
    r = readElement(data, size_t(std::min<uint64_t>(segAt, len)), len, seg);
    if (r == Read::Short) return needUpTo(segAt + MAX_HEADER);
    if (r == Read::Bad || seg.id != ID_SEGMENT) return status(CuesStatus::Invalid);

    const size_t segEnd = seg.size == UNKNOWN_SIZE ? len : size_t(std::min<uint64_t>(seg.data + seg.size, len));
    uint64_t scale = 1000000;   // ns per timecode unit (WebM default: 1 ms)
    double duration = 0;        // timecode units
    // Found and NotFound carry the duration read so far.
    auto withDuration = [&](WebmCues out) {
        const double ms = duration * double(scale) / 1e6;
        if (ms > 0 && ms < double(UINT32_MAX)) out.duration_ms = uint32_t(ms + 0.5);
        return out;
    };
    size_t p = seg.data;
    while (true) {
        Element e;
        r = readElement(data, p, segEnd, e);
        if (r == Read::Short) {
            if (seg.size != UNKNOWN_SIZE && p >= seg.data + seg.size) return withDuration(status(CuesStatus::NotFound));
            return needUpTo(p + MAX_HEADER);
        }
        if (r == Read::Bad) return status(CuesStatus::Invalid);
        if (e.id == ID_CLUSTER) return withDuration(status(CuesStatus::NotFound));   // audio starts: no index up front
        if (e.size == UNKNOWN_SIZE) return status(CuesStatus::Invalid);

        const uint64_t end = e.data + e.size;
        if (e.id == ID_CUES) {
            if (end > len) return needUpTo(end);
            std::vector<RawCue> raw;
            if (!parseCues(data, e, raw)) return status(CuesStatus::Invalid);
            if (raw.empty()) return withDuration(status(CuesStatus::NotFound));

            WebmCues out;
            out.cues.reserve(raw.size());
            for (const RawCue& c : raw) {
                const uint64_t ms = c.time * scale / 1000000;
                const uint64_t offset = seg.data + c.position;
                if (ms > UINT32_MAX || offset > UINT32_MAX) return status(CuesStatus::Invalid);
                if (!out.cues.empty()) {
                    const CuePoint& prev = out.cues.back();
                    if (offset == prev.offset) continue;   // another cue in the same cluster
                    if (offset < prev.offset || ms < prev.time_ms) return status(CuesStatus::Invalid);
                }
                out.cues.push_back({uint32_t(ms), uint32_t(offset)});
            }
            out.status = CuesStatus::Found;
            return withDuration(std::move(out));
        }
        if (e.id == ID_INFO) {
            if (end > len) return needUpTo(end);
            bool ok = forEachChild(data, e.data, size_t(end), [&](const Element& f) {
                if (f.id == ID_TIMECODE_SCALE) return readUint(data, f, scale) && scale > 0;
                // A bad Duration only loses the length, not the index.
                if (f.id == ID_DURATION && (!readFloat(data, f, duration) || !(duration > 0))) duration = 0;
                return true;
            });
            if (!ok) return status(CuesStatus::Invalid);
        }
        p = size_t(end);   // past the prefix: the next read asks for more
    }
}

const CuePoint& cueAtOrBefore(const std::vector<CuePoint>& cues, uint32_t time_ms) {
    auto it = std::upper_bound(cues.begin(), cues.end(), time_ms,
                               [](uint32_t t, const CuePoint& c) { return t < c.time_ms; });
    return it == cues.begin() ? *it : *(it - 1);
}

SeekAction seekAction(uint32_t block_ms, uint32_t target_ms) {
    if (block_ms >= target_ms) return SeekAction::Play;
    if (target_ms - block_ms <= SEEK_PREROLL_MS) return SeekAction::Preroll;
    return SeekAction::Skip;
}

bool readBlockTimecode(const uint8_t* payload, size_t len, int16_t& relative) {
    if (len == 0) return false;
    size_t trackLen = 1;
    while (trackLen <= 8 && !(payload[0] & (0x80 >> (trackLen - 1)))) ++trackLen;
    if (trackLen > 8 || trackLen + 3 > len) return false;
    relative = int16_t(uint16_t(payload[trackLen]) << 8 | payload[trackLen + 1]);
    return true;
}

ClusterScan findCluster(const uint8_t* data, size_t len) {
    static constexpr uint8_t id[4] = {ID_CLUSTER >> 24, (ID_CLUSTER >> 16) & 0xFF, (ID_CLUSTER >> 8) & 0xFF,
                                      ID_CLUSTER & 0xFF};
    for (size_t i = 0; i + sizeof(id) <= len; ++i) {
        if (memcmp(data + i, id, sizeof(id)) != 0) continue;
        Element cluster;
        const Read r = readElement(data, i, len, cluster);
        if (r == Read::Short || (r == Read::Ok && cluster.data >= len)) return {false, i};
        if (r == Read::Ok && data[cluster.data] == ID_CLUSTER_TIMECODE) return {true, i};
    }
    // Keep a tail that could be the start of a split ID.
    return {false, len > sizeof(id) - 1 ? len - (sizeof(id) - 1) : 0};
}

} // namespace Media
