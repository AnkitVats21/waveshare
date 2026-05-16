#pragma once

#include "driver/i2c_master.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include <cstdint>

/**
 * @brief HAL driver for the audio subsystem of the Waveshare ESP32-S3 board.
 *
 * Owns and manages:
 *  - I2S TDM channel pair (TX + RX) via I2S_NUM_1
 *  - ES7210 ADC codec (record path)
 *  - ES8311 DAC codec (playback path)
 *  - SPIRAM TDM work buffer for AEC frame extraction
 *
 * AudioHal does NOT know about:
 *  - Application audio modes (wake word, streaming, AEC policy)
 *  - RTP, buffering, or FreeRTOS tasks
 *  - Board-level hardware policies
 *
 * Board owns one AudioHal instance and initializes it via init().
 * Board's public methods delegate to AudioHal for all audio operations.
 */
class AudioHal {
public:
  /**
   * @brief Interleaved mic + reference frame for AEC processing.
   *
   * Channel mapping for Waveshare ESP32-S3 Audio:
   *   Slot 0 → Mic 1 (primary voice)
   *   Slot 1 → Mic 2
   *   Slot 3 → Speaker loopback (AEC reference)
   */
  struct AecFrame {
    int16_t mic; // Primary voice channel
    int16_t ref; // Speaker loopback reference
  };

  /**
   * @brief Configuration passed from Board before initialization.
   *
   * Board fills this from its own pre-init settings and passes the I2C
   * bus handle so AudioHal can bind codec control interfaces to it.
   */
  struct Config {
    uint32_t                sample_rate   = 16000;
    int                     record_volume = 70;
    int                     play_volume   = 80;
    i2c_master_bus_handle_t i2c_bus       = nullptr;
  };

  // --- Lifecycle ------------------------------------------------------------

  /**
   * @brief Initialize I2S channels and both codec devices.
   * @param cfg Configuration (sample rate, volumes, I2C bus handle)
   * @return ESP_OK on success
   */
  esp_err_t init(const Config &cfg);

  /**
   * @brief Close and delete all codec devices and I2S channels.
   * @return ESP_OK on success
   */
  esp_err_t deinit();

  /**
   * @brief Tear down and reinitialize audio at a new sample rate.
   * @param sample_rate New sample rate in Hz
   * @return ESP_OK on success
   */
  esp_err_t reinit(uint32_t sample_rate);

  // --- Audio frame retrieval ------------------------------------------------

  /**
   * @brief Read mono microphone samples directly into caller's buffer.
   * @param buffer     Destination buffer for int16_t PCM samples
   * @param buffer_len Number of samples to read
   * @return ESP_OK on success
   */
  esp_err_t getFeedData(int16_t *buffer, int buffer_len);

  /**
   * @brief Read interleaved AEC frames (mic + reference) from the TDM bus.
   * @param frames     Destination array of AecFrame structs
   * @param num_frames Number of frames to read
   * @return ESP_OK on success
   */
  esp_err_t getAecFrames(AecFrame *frames, int num_frames);

  // --- Volume / Gain control ------------------------------------------------

  esp_err_t setPlayVolume(int volume);
  esp_err_t getPlayVolume(int *volume);
  esp_err_t setRecordGain(float db_value);

  // --- Handle accessors (for Board to satisfy HardwareAudioHandles) ----------

  i2s_chan_handle_t      getTxHandle()  const { return m_tx_handle;  }
  i2s_chan_handle_t      getRxHandle()  const { return m_rx_handle;  }
  esp_codec_dev_handle_t getPlayDev()   const { return m_play_dev;   }
  esp_codec_dev_handle_t getRecordDev() const { return m_record_dev; }

  bool isInitialized() const { return m_initialized; }

private:
  esp_err_t initI2s(uint32_t sample_rate);
  esp_err_t initCodecs(uint32_t sample_rate);

  i2c_master_bus_handle_t m_i2c_bus        = nullptr;
  i2s_chan_handle_t       m_tx_handle       = nullptr;
  i2s_chan_handle_t       m_rx_handle       = nullptr;
  esp_codec_dev_handle_t  m_play_dev        = nullptr;
  esp_codec_dev_handle_t  m_record_dev      = nullptr;
  int16_t                *m_tdm_work_buffer = nullptr;
  uint32_t                m_sample_rate     = 16000;
  int                     m_record_volume   = 70;
  int                     m_play_volume     = 80;
  bool                    m_initialized     = false;

  static constexpr size_t TDM_BUF_SIZE = 1024 * 4;
  static constexpr const char *TAG     = "AudioHal";
};
