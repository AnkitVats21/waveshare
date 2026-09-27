#pragma once

#include <cstddef>
#include <cstdint>

// Reads a recording's length and format from its own bytes, for
// recordings.ndb. No file access: the caller passes the first and last bytes
// of the file. Pure, so the host tests cover it.
namespace RecordingProbe {

struct Info {
    uint32_t duration_ms = 0;
    uint32_t sample_rate = 0;  // Opus: the input rate in OpusHead; WAV: the sample rate
    uint8_t channels = 0;
};

// The first bytes a probe needs, and the last bytes an Ogg probe searches
// for the final page (a page is at most 65307 bytes; ours are a few KB).
constexpr size_t HEAD_BYTES = 512;
constexpr size_t TAIL_BYTES = 16 * 1024;

// Ogg Opus: OpusHead from `head`, length from the granule of the last whole
// page in `tail` (a cut-off final page, as after a power cut, is skipped).
// False if `head` isn't Ogg Opus or `tail` holds no whole page.
bool probeOpus(const uint8_t* head, size_t head_len, const uint8_t* tail, size_t tail_len, Info& out);

// WAV (PCM): length from the data chunk. A data size of 0 or past the end
// of the file (a recording that never finished) uses the bytes present.
bool probeWav(const uint8_t* head, size_t head_len, uint64_t file_size, Info& out);

// Ogg CRC-32 (polynomial 0x04C11DB7, not reflected), over a page with its
// CRC field zeroed.
uint32_t oggCrc(const uint8_t* data, size_t len, uint32_t crc = 0);

}  // namespace RecordingProbe
