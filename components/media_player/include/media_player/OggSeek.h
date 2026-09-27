#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Media {

/**
 * @brief Seeking in Ogg Opus files. Portable (host-tested).
 *
 * Ogg has no index, so a seek starts reading at an estimated byte position
 * (before the target), finds the next page by its capture pattern and CRC,
 * and drops pages that end before the target. The decoder, reset for the
 * seek, is given the stream's header pages again first, renumbered so they
 * run straight into the first kept page: the demuxer checks that page
 * numbers follow on.
 */

// Ogg's CRC-32 (polynomial 0x04C11DB7, no reflection, no final xor).
uint32_t oggCrc(const uint8_t* data, size_t len, uint32_t crc = 0);

constexpr uint8_t OGG_CONTINUED = 0x01;   // header_type: the page continues a packet
constexpr uint8_t OGG_EOS = 0x04;         // header_type: the last page
constexpr size_t OGG_MAX_PAGE = 27 + 255 + 255 * 255;

struct OggPage {
    uint8_t flags;
    uint32_t serial;
    uint32_t seq;
    int64_t granule;   // -1: no packet ends on the page
    size_t len;        // header, lacing and body
};

enum class OggPageRead : uint8_t {
    Ok,         // `page` describes a whole page with a matching CRC
    NeedMore,   // could be a page, but `avail` bytes don't hold all of it
    NotPage,    // no page starts here
};

OggPageRead readOggPage(const uint8_t* p, size_t avail, OggPage& page);

// 48 kHz frames in an Opus packet, from its TOC byte (RFC 6716 section 3.1).
uint32_t opusPacketFrames(const uint8_t* pkt, size_t len);

// Granule position where the page's first packet starts: its granule less
// the frames of the packets ending on it. -1 for a page that continues a
// packet, has no granule, or is the last (its granule may trim the end).
int64_t oggPageStartGranule(const uint8_t* p, const OggPage& page);

// The pages before the audio (OpusHead, OpusTags), copied from the start of
// the file.
struct OggHeader {
    std::vector<uint8_t> bytes;
    uint32_t serial = 0;
    uint16_t pre_skip = 0;
    uint32_t pages = 0;
};

// Reads the header from the first `len` bytes of the file. False if they
// don't reach the first audio page, or aren't Ogg Opus.
bool parseOggHeader(const uint8_t* data, size_t len, OggHeader& out);

// The header with its pages numbered so the last is first_audio_seq - 1
// (CRCs updated). Empty if first_audio_seq is too small.
std::vector<uint8_t> renumberHeader(const OggHeader& header, uint32_t first_audio_seq);

// Finds where to start decoding for a seek, in bytes read from an estimated
// position. Call scan() with the unread bytes (after those it said to drop)
// until found().
class OggSeekScanner {
public:
    OggSeekScanner(uint32_t serial, uint16_t pre_skip, uint32_t target_ms);

    // Returns how many leading bytes of `buf` to drop. Once found(), the
    // bytes after those begin the first page to decode.
    size_t scan(const uint8_t* buf, size_t len);

    bool found() const { return _found; }
    uint32_t startSeq() const { return _startSeq; }
    // 48 kHz frames to throw away after the decoder's pre-skip, so the
    // output starts at the target.
    uint32_t discardFrames() const { return _discard; }

private:
    uint32_t _serial;
    uint16_t _preSkip;
    int64_t _target;              // 48 kHz frames
    int64_t _prevEnd = -1;        // granule of the page just before, -1 if not seen
    bool _found = false;
    uint32_t _startSeq = 0;
    uint32_t _discard = 0;
};

} // namespace Media
