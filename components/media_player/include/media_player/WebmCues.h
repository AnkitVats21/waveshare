#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Media {

/**
 * @brief The seek index (Cues) of a WebM file, read from the file's first bytes.
 *
 * YouTube audio files carry one cue per Cluster (about every 10 s) before the
 * first Cluster, so the first few KB index the whole track
 * (docs/nexus-db-design.md, "Seeking"). Portable (host-tested).
 */
struct CuePoint {
    uint32_t time_ms;   // cluster start, from the track start
    uint32_t offset;    // byte position of the Cluster element in the file
};

enum class CuesStatus : uint8_t {
    Found,      // `cues` holds the index
    NeedMore,   // the prefix ends inside the header or the Cues: read `need` bytes and retry
    NotFound,   // no Cues before the first Cluster (other muxers put them at the end)
    Invalid,    // not WebM, or a malformed or out-of-order index
};

struct WebmCues {
    CuesStatus status = CuesStatus::NotFound;
    std::vector<CuePoint> cues;   // ascending time and offset
    uint32_t need = 0;            // NeedMore: a prefix length that gets further
};

// Longest index accepted: 64 KB of cues, about 22 h of audio at 10 s per cluster.
constexpr size_t MAX_CUES = 8192;
// Largest prefix NeedMore asks for; a header bigger than this is Invalid.
constexpr uint32_t MAX_HEADER_BYTES = 1024 * 1024;

// Reads the Cues from `data`, the first `len` bytes of a WebM file.
WebmCues parseWebmCues(const uint8_t* data, size_t len);

// The last cue at or before `time_ms`, or the first cue if `time_ms` precedes
// it. `cues` must not be empty.
const CuePoint& cueAtOrBefore(const std::vector<CuePoint>& cues, uint32_t time_ms);

} // namespace Media
