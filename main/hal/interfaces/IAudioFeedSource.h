#pragma once

#include "esp_err.h"
#include <cstdint>

/**
 * @file IAudioFeedSource.h
 * @brief Abstract interface for reading raw multi-channel mic frames.
 *
 * This is the only hardware dependency of WakeWordDetector.
 * The concrete implementation (AudioHal via Board) satisfies this interface.
 * In tests a mock can substitute.
 */
class IAudioFeedSource {
public:
    /**
     * @brief Read one chunk of raw 4-channel interleaved mic data.
     *
     * @param buf       Destination buffer (must hold at least bytes bytes)
     * @param bytes     Number of bytes to read (= chunksize * channels * sizeof(int16_t))
     * @return ESP_OK on success
     */
    virtual esp_err_t readFeedData(int16_t *buf, int bytes) = 0;

    /**
     * @brief Return the number of interleaved channels provided per frame.
     * Typically 4 (RMNM layout: Ref, Mic1, Noise, Mic2).
     */
    virtual int feedChannelCount() const = 0;

    /**
     * @brief Return the AFE input format string (e.g. "RMNM").
     * Used to configure the AFE engine at init time.
     */
    virtual const char *feedInputFormat() const = 0;

    virtual ~IAudioFeedSource() = default;
};
