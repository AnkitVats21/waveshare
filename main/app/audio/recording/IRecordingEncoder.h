#pragma once

#include <cstdint>
#include <cstddef>

namespace sd_storage { class File; }

/**
 * @brief Abstract sink for a recording session's PCM samples.
 *
 * The writer task owns the output file and drives one of these per
 * recording. OggOpusEncoder is the only implementation today.
 */
class IRecordingEncoder {
public:
    virtual ~IRecordingEncoder() = default;

    /** Write the container header and record the input stream parameters. */
    virtual bool open(sd_storage::File& file, uint32_t input_rate, int channels) = 0;

    /** Encode one chunk of interleaved 16-bit PCM at the input rate. */
    virtual bool writeSamples(const int16_t* data, size_t frame_count) = 0;

    /** Flush buffered audio and trailing container data. Does not close the file. */
    virtual bool finalize() = 0;
};
