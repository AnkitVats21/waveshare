#pragma once

#include "app/audio/recording/IRecordingEncoder.h"
#include "app/audio/recording/PolyphaseResampler.h"

struct OpusEncoder;

/**
 * @brief Encodes PCM to Opus and writes a standard Ogg Opus (.opus) file.
 *
 * Input at a rate Opus doesn't take (the 32 kHz mic feed) is resampled to
 * `encode_rate` first. Pages are written about once a second and the file is
 * synced every ~10 s, so a recording cut short by a reset stays playable up
 * to the last page. Encoder state and buffers live in PSRAM.
 */
class OggOpusEncoder : public IRecordingEncoder {
public:
    struct Config {
        uint32_t encode_rate;  // 8000, 12000, 16000, 24000 or 48000
        int      bitrate;      // bits per second, all channels
        bool     voice;        // hint that the signal is speech
        int      complexity;   // 0-10; lower is cheaper on CPU
    };

    explicit OggOpusEncoder(const Config& cfg) : m_cfg(cfg) {}
    ~OggOpusEncoder() override;

    bool open(sd_storage::File& file, uint32_t input_rate, int channels) override;
    bool writeSamples(const int16_t* data, size_t frame_count) override;
    bool finalize() override;

    /** Microseconds spent resampling and encoding so far (for CPU logging). */
    int64_t busyMicros() const { return m_busy_us; }

private:
    bool feed(const int16_t* pcm, size_t frames);
    bool encodeFrame();
    void addPacket(const uint8_t* data, size_t len);
    bool flushPage(bool eos, int64_t granule);
    bool writePage(uint8_t header_type, int64_t granule,
                   const uint8_t* lacing, size_t nseg, const uint8_t* body, size_t body_len);

    Config m_cfg;
    sd_storage::File* m_file = nullptr;
    OpusEncoder* m_enc = nullptr;
    PolyphaseResampler m_resampler;
    bool m_resample = false;
    int m_channels = 1;

    // PSRAM buffers
    int16_t* m_rs_out = nullptr;     // resampler output
    size_t   m_rs_out_cap = 0;       // frames
    int16_t* m_frame = nullptr;      // one 20 ms Opus frame being filled
    size_t   m_frame_fill = 0;       // frames
    size_t   m_frame_size = 0;       // frames per 20 ms at encode_rate
    uint8_t* m_packet = nullptr;     // encoder output
    uint8_t* m_body = nullptr;       // current Ogg page body
    size_t   m_body_len = 0;
    uint8_t  m_lacing[255] = {};
    size_t   m_nseg = 0;
    int      m_page_packets = 0;
    bool     m_finalizing = false;

    uint32_t m_serial = 0;
    uint32_t m_page_seq = 0;
    int      m_pages_since_sync = 0;
    uint32_t m_pre_skip = 0;          // 48 kHz samples
    uint64_t m_packets = 0;
    uint64_t m_input_frames = 0;      // real frames at encode_rate
    int64_t  m_busy_us = 0;
};
