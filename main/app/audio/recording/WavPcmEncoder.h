#pragma once

#include "app/audio/recording/IRecordingEncoder.h"

/** Canonical 44-byte RIFF/PCM WAV writer. */
class WavPcmEncoder : public IRecordingEncoder {
public:
    bool open(FILE* stream, uint32_t sample_rate, int channels, int bits_per_sample) override;
    bool writeSamples(const int16_t* data, size_t frame_count) override;
    bool finalize() override;

private:
    FILE*    m_stream         = nullptr;
    int      m_channels       = 1;
    int      m_bits_per_sample = 16;
    uint32_t m_bytes_written  = 0;
};
