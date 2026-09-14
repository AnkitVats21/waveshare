#pragma once

#include "common/AudioRates.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <cstdint>
#include <cstddef>

namespace btplayer {

class BtPlayerI2s {
public:
    struct Config {
        int      port         = 0; // I2S_NUM_0
        int      bclk_pin     = 4;
        int        ws_pin      = 5;
        int        dout_pin    = 6;
        uint32_t   sample_rate = COMPANION_SAMPLE_RATE;
        size_t     dma_desc_num = 8;
        size_t     dma_frame_num = 256;
    };

    static BtPlayerI2s& getInstance();

    esp_err_t init(const Config& config);
    void deinit();

    /**
     * @brief Write 16-bit interleaved stereo samples (L, R) to companion I2S.
     * @param stereo_pcm Pointer to interleaved int16_t array (size = frame_count * 2)
     * @param frame_count Number of stereo frames (pairs of samples)
     * @param timeout_ms Timeout in milliseconds (default 1000 ms)
     * @return Number of frames actually written
     */
    size_t writeSamples(const int16_t* stereo_pcm, size_t frame_count, uint32_t timeout_ms = 1000);

    /**
     * @brief Write zero-silence frames to keep continuous BCLK and WS clocking active.
     * @param frame_count Number of stereo silence frames to write
     * @param timeout_ms Timeout in milliseconds (default 1000 ms)
     * @return Number of frames actually written
     */
    size_t writeSilence(size_t frame_count, uint32_t timeout_ms = 1000);

    bool isInitialized() const { return m_initialized; }

private:
    BtPlayerI2s() = default;
    ~BtPlayerI2s();
    BtPlayerI2s(const BtPlayerI2s&) = delete;
    BtPlayerI2s& operator=(const BtPlayerI2s&) = delete;

    i2s_chan_handle_t m_tx_handle{nullptr};
    bool              m_initialized{false};

    static constexpr size_t SILENCE_CHUNK_FRAMES = 256;
    int16_t           m_silence_buf[SILENCE_CHUNK_FRAMES * 2]{0};

    static constexpr const char* TAG = "BtPlayerI2s";
};

} // namespace btplayer
