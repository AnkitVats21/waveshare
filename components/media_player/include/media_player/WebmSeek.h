#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Media {

/**
 * @brief Seeking in WebM files (docs/nexus-db-design.md, "Seeking").
 * Portable (host-tested).
 *
 * The seek index (Cues): YouTube audio files carry one cue per Cluster (about
 * every 10 s) before the first Cluster, so the file's first few KB index the
 * whole track. A seek starts reading at the cluster at or before the target,
 * then the decoder skips blocks up to the target (seekAction).
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

// ── Exact seek inside a cluster ─────────────────────────────────────────────

// Opus needs about 80 ms of decoded audio to settle after a jump (RFC 7845,
// section 4.6); those frames are decoded and thrown away.
constexpr uint32_t SEEK_PREROLL_MS = 80;

enum class SeekAction : uint8_t {
    Skip,      // before the pre-roll: drop the block without decoding it
    Preroll,   // decode it to settle the decoder, discard the audio
    Play,      // at or after the target: the seek is done
};

// What to do with a block starting at `block_ms` while seeking to `target_ms`.
SeekAction seekAction(uint32_t block_ms, uint32_t target_ms);

// A SimpleBlock payload starts with the track number (EBML vint), a signed
// 16-bit timecode relative to its cluster, and a flags byte. Returns false if
// `len` bytes do not hold that header.
bool readBlockTimecode(const uint8_t* payload, size_t len, int16_t& relative);

// ── Starting anywhere in the file ───────────────────────────────────────────

struct ClusterScan {
    bool found;
    size_t at;   // found: the Cluster ID; otherwise the bytes before `at` can be dropped
};

// Finds the first Cluster in bytes read from any point of a WebM file (a seek
// by estimate lands mid-cluster): the Cluster ID, a size, then the cluster's
// Timecode, which muxers write as its first child.
ClusterScan findCluster(const uint8_t* data, size_t len);

} // namespace Media
