#pragma once

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"

/**
 * @brief Low-level hardware handles for the audio I2S and codec peripherals.
 *
 * Populated by Board::begin() and stored in SystemContext.
 * Only HAL-layer code and AudioService should read these directly.
 */
struct HardwareAudioHandles {
  i2s_chan_handle_t mic_rx_handle     = nullptr;
  i2s_chan_handle_t speaker_tx_handle = nullptr;
  esp_codec_dev_handle_t play_dev     = nullptr;
  esp_codec_dev_handle_t record_dev   = nullptr;
};
