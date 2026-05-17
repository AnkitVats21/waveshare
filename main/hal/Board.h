#pragma once

#include "esp_err.h"
#include "esp_io_expander.h"
#include "hal/Board_defs.h"
#include "hal/HalBase.h"
#include "hal/audio/AudioHal.h"
#include "hal/io/I2CBus.h"
#include "hal/io/IoExpander.h"
#include "hal/led/LedStripManager.h"
#include "hal/storage/SdCardManager.h"
#include <cstdint>

/**
 * @brief Hardware orchestration layer for the Waveshare ESP32-S3 Audio Board.
 *
 * Board owns all concrete HAL component instances and coordinates their
 * initialization in the correct hardware-dependent order.
 *
 * Responsibilities (what Board DOES):
 *  - Owns: AudioHal, I2CBus, IoExpander, LedStripManager, SdCardManager
 *  - Initializes hardware in correct dependency order
 *  - Exposes unified, policy-aware APIs to the application layer
 *  - Delegates all driver-level work to HAL components
 *
 * Non-responsibilities (what Board does NOT do):
 *  - Contain raw driver logic or codec register sequences
 *  - Contain LED or SD card implementation details
 *  - Contain application streaming, RTP, or wake word logic
 */
class Board : public HalBase {
public:
  static Board &getInstance();

  /**
   * @brief Initialize all board peripherals in dependency order.
   * @return true on success
   */
  bool begin() override;

  // -- Pre-init configuration (call before begin()) --------------------------
  void setSampleRate(uint32_t sample_rate) { m_sample_rate = sample_rate; }
  void setInitialVolumes(int record, int play) {
    m_record_volume = record;
    m_play_volume = play;
  }
  void setPreviousVolume() { m_audio.setPreviousVolume(); }

  // -- Handle accessors (for HardwareAudioHandles population in main.cpp) ----
  i2c_master_bus_handle_t getI2cBus() { return m_i2c.getBusHandle(); }
  i2s_chan_handle_t getTxHandle() { return m_audio.getTxHandle(); }
  i2s_chan_handle_t getRxHandle() { return m_audio.getRxHandle(); }
  esp_codec_dev_handle_t getPlayDev() { return m_audio.getPlayDev(); }
  esp_codec_dev_handle_t getRecordDev() { return m_audio.getRecordDev(); }
  esp_io_expander_handle_t getIoExpander() { return m_io.getRawHandle(); }

  // -- LED orchestration (delegates to LedStripManager) ----------------------
  void setLedPixel(uint32_t index, uint32_t r, uint32_t g, uint32_t b) {
    m_leds.setPixel(index, r, g, b);
  }
  void setAllLedsColor(uint32_t r, uint32_t g, uint32_t b) {
    m_leds.setAll(r, g, b);
  }
  void refreshLeds() { m_leds.refresh(); }
  void clearLeds() { m_leds.clear(); }

  // -- Audio orchestration APIs (delegate to AudioHal) -----------------------

  /**
   * @brief Read 4-channel interleaved mic data.
   *
   * @param is_get_raw_channel  true  → raw 4-ch RMNM (for AFE/wake-word feed)
   *                            false → remapped 3-ch [Mic1, Mic2, Ref]
   * @param buffer     Destination int16_t buffer
   * @param buffer_len Byte count to read
   */
  esp_err_t getFeedData(bool is_get_raw_channel, int16_t *buffer,
                        int buffer_len) {
    return m_audio.getFeedData(is_get_raw_channel, buffer, buffer_len);
  }

  /**
   * @brief Convenience overload — raw 4-ch, for streaming pipeline.
   */
  esp_err_t getFeedData(int16_t *buffer, int buffer_len) {
    return m_audio.getFeedData(buffer, buffer_len);
  }

  /** @brief Number of interleaved channels returned by getFeedData. */
  int getFeedChannel() const { return m_audio.getFeedChannel(); }

  /** @brief AFE input format string ("RMNM"). */
  const char *getInputFormat() const { return m_audio.getInputFormat(); }

  /**
   * @brief Write PCM audio to the ES8311 playback codec.
   * @param data   16-bit mono PCM source buffer
   * @param length Byte length of source buffer
   */
  esp_err_t audioPlay(const int16_t *data, int length) {
    return m_audio.audioPlay(data, length);
  }

  esp_err_t setPlayVolume(int volume) { return m_audio.setPlayVolume(volume); }
  esp_err_t getPlayVolume(int *volume) { return m_audio.getPlayVolume(volume); }
  esp_err_t setRecordGain(float db_value, bool force = false);

  /**
   * @brief Tear down I2S and codec hardware (called before reinitAudio).
   */
  esp_err_t deinitAudio() { return m_audio.deinit(); }

  /**
   * @brief Reinitialize audio subsystem at a new sample rate.
   * Board guards against calling reinit on an uninitialized board.
   */
  esp_err_t reinitAudio(uint32_t sample_rate);

  // -- Storage (delegates to SdCardManager) ----------------------------------
  esp_err_t initSdCard(const char *mount_point, size_t max_files) {
    return m_storage.mount(mount_point, max_files);
  }

private:
  Board() = default;
  ~Board() = default;

  // HAL component instances — Board is the sole owner
  I2CBus m_i2c;
  IoExpander m_io;
  AudioHal m_audio;
  LedStripManager m_leds;
  SdCardManager m_storage;

  // Pre-init settings (forwarded to AudioHal::Config on begin())
  uint32_t m_sample_rate = 16000;
  int m_record_volume = 70;
  int m_play_volume = 80;
  float m_current_mic_gain = -999.0f; // Track active mic gain value

  static constexpr const char *TAG = "Board";
};
