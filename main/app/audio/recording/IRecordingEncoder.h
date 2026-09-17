#pragma once

#include <cstdio>
#include <cstdint>
#include <cstddef>

/**
 * @brief Abstract sink for a recording session's PCM samples.
 *
 * The writer task owns a FILE* (opened via StorageService) and drives one
 * of these per recording. WavPcmEncoder is the only implementation today;
 * a future compressed codec (Opus, etc.) can implement the same interface
 * without touching AudioRecorder or the writer task.
 */
class IRecordingEncoder {
public:
    virtual ~IRecordingEncoder() = default;

    /** Write the container header (if any) and record stream parameters. */
    virtual bool open(FILE* stream, uint32_t sample_rate, int channels, int bits_per_sample) = 0;

    /** Encode/stream one chunk of interleaved 16-bit PCM samples. */
    virtual bool writeSamples(const int16_t* data, size_t frame_count) = 0;

    /** Patch/flush any trailing container metadata. Does not close the stream. */
    virtual bool finalize() = 0;
};
