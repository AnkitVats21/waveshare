#pragma once

#include "audio_codec_ctrl_if.h"
#include "audio_codec_data_if.h"
#include "audio_codec_gpio_if.h"
#include "audio_codec_if.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "hal/audio/IAudioFeedSource.h"
#include <cstdint>

/**
 * @brief HAL driver for the audio subsystem of the Waveshare ESP32-S3 board.
 *
 * Owns and manages:
 *  - I2S STD channel pair (TX + RX) via I2S_NUM_1
 *  - ES7210 ADC codec (record path — 4 channels for AFE/wake-word)
 *  - ES8311 DAC codec (playback path)
 *  - SPIRAM 4-ch work buffer used by getFeedData()
 *
 * AudioHal does NOT know about:
 *  - Application audio modes (wake word, streaming, AEC policy)
 *  - RTP, buffering, or FreeRTOS tasks
 *  - Board-level hardware policies
 *
 * I2S mode note:
 *   The hardware uses a SINGLE I2S_STD bus (I2S_NUM_1) shared by both the
 *   ES8311 DAC (playback) and ES7210 ADC (record).  Both codecs are clocked
 *   by the same BCLK/WS pair.  The I2S bus is opened in STEREO 32-bit mode
 *   so the ES7210 can deliver 4 channels via standard left/right framing
 *   (matching the verified Waveshare demo BSP).
 *
 * Board owns one AudioHal instance and initializes it via init().
 * Board's public methods delegate to AudioHal for all audio operations.
 */
class AudioHal : public IAudioFeedSource {
public:
  /**
   * @brief Configuration passed from Board before initialization.
   */
  struct Config {
    uint32_t sample_rate = 24000;
    int record_volume = 70;
    int play_volume = 80;
    i2c_master_bus_handle_t i2c_bus = nullptr;
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

  /**
   * @brief Dynamically reconfigure physical standard I2S clock without heap re-allocations.
   * @param sample_rate New sample rate in Hz (e.g. 16000 or 24000)
   * @return ESP_OK on success
   */
  esp_err_t setHardwareSampleRate(uint32_t sample_rate);

  // --- Audio frame retrieval ------------------------------------------------

  /**
   * @brief Read audio data from the 4-ch ES7210 ADC into caller's buffer.
   *
   * This is the primary capture API used by both the streaming pipeline and
   * the AFE/wake-word engine.
   *
   * @param is_get_raw_channel
   *   true  → return all 4 raw interleaved int16 channels as-is (RMNM order).
   *            Required by the AFE feed task.
   *   false → remap to a 3-channel layout: [Mic1, Mic2, Ref] per frame.
   *            Useful for simplified AEC pipelines.
   *
   * @param buffer     Destination int16_t buffer.
   * @param buffer_len Total byte count to read (must be a multiple of
   *                   feed_channel * sizeof(int16_t) * chunksize).
   * @return ESP_OK on success
   */
  esp_err_t getFeedData(bool is_get_raw_channel, int16_t *buffer,
                        int buffer_len);

  /**
   * @brief Convenience overload: reads raw 4-channel data (equivalent to
   *        getFeedData(true, buffer, buffer_len)).
   *        Used by the streaming pipeline and MicCapture task which only
   *        need the first (primary mic) channel after the call.
   */
  esp_err_t getFeedData(int16_t *buffer, int buffer_len);

  /**
   * @brief Return number of interleaved channels the ES7210 delivers.
   * @return 4 (always — hardware provides RMNM 4-channel interleaved)
   */
  int getFeedChannel() const { return ADC_I2S_CHANNEL; }

  // --- IAudioFeedSource interface (implemented here; used by WakeWordEngine) ---

  /**
   * @brief Read one chunk of raw 4-channel mic data (IAudioFeedSource impl).
   * Delegates to getFeedData(true, buf, bytes).
   */
  esp_err_t readFeedData(int16_t* buf, int bytes) override {
      return getFeedData(true, buf, bytes);
  }

  /** @brief Returns 4 (RMNM: Ref, Mic1, Noise, Mic2). */
  int feedChannelCount() const override { return ADC_I2S_CHANNEL; }

  /** @brief Returns "RMNM" — the AFE input format string for this board. */
  const char* feedInputFormat() const override { return "RMNM"; }

  // --- Playback (write to ES8311 DAC) ---------------------------------------

  /**
   * @brief Write PCM audio data to the ES8311 playback codec.
   *
   * Input is always 16-bit mono @ m_sample_rate.  The function up-samples
   * to stereo 32-bit as required by the shared I2S bus.
   *
   * @param data   Source int16_t PCM buffer (16-bit, mono, m_sample_rate)
   * @param length Byte length of the source buffer
   * @param ticks_to_wait  FreeRTOS ticks to wait for DMA space (unused, kept
   *                       for API compatibility with the demo BSP)
   * @return ESP_OK on success
   */
  esp_err_t audioPlay(const int16_t *data, int length,
                      uint32_t ticks_to_wait = 0);

  // --- Volume / Gain control ------------------------------------------------

  esp_err_t setPlayVolume(int volume);
  esp_err_t setPreviousVolume();
  esp_err_t getPlayVolume(int *volume);
  esp_err_t setRecordGain(float db_value);

  // --- Record state control (for pausing/resuming RX DMA) ------------------
  esp_err_t pauseRecord();
  esp_err_t resumeRecord();

  uint32_t getSampleRate() const { return m_sample_rate; }
  int getPlayVolume() const { return m_play_volume; }
  float getRecordGain() const { return (float)m_record_volume; }

  // --- Handle accessors (for Board to satisfy HardwareAudioHandles) ----------

  i2s_chan_handle_t getTxHandle() const { return m_tx_handle; }
  i2s_chan_handle_t getRxHandle() const { return m_rx_handle; }
  esp_codec_dev_handle_t getPlayDev() const { return m_play_dev; }
  esp_codec_dev_handle_t getRecordDev() const { return m_record_dev; }

  bool isInitialized() const { return m_initialized; }

private:
  esp_err_t initI2s(uint32_t sample_rate);
  esp_err_t initCodecs(uint32_t sample_rate);

  i2c_master_bus_handle_t m_i2c_bus = nullptr;
  i2s_chan_handle_t m_tx_handle = nullptr;
  i2s_chan_handle_t m_rx_handle = nullptr;
  esp_codec_dev_handle_t m_play_dev = nullptr;
  esp_codec_dev_handle_t m_record_dev = nullptr;
  uint32_t m_sample_rate = 24000;
  int m_record_volume = 70;
  int m_play_volume = 80;
  int m_play_previous_volume = 80;
  bool m_initialized = false;

  // Codec interface objects — must be deleted in deinit() to prevent
  // stale I2S handle pointers surviving into the next reinit cycle.
  const audio_codec_data_if_t *m_record_data_if = nullptr;
  const audio_codec_ctrl_if_t *m_record_ctrl_if = nullptr;
  const audio_codec_if_t *m_record_codec_if = nullptr;
  const audio_codec_data_if_t *m_play_data_if = nullptr;
  const audio_codec_ctrl_if_t *m_play_ctrl_if = nullptr;
  const audio_codec_gpio_if_t *m_play_gpio_if = nullptr;
  const audio_codec_if_t *m_play_codec_if = nullptr;

  // Number of interleaved int16 channels the ES7210 streams over I2S
  static constexpr int ADC_I2S_CHANNEL = 4;
  static constexpr const char *TAG = "AudioHal";
};
