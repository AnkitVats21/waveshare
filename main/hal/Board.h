#pragma once

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_io_expander.h"
#include "hal/Board_defs.h"
#include "hal/HalBase.h"
#include "led_strip.h"

/**
 * @brief Board class to handle hardware-level initialization and access.
 * Manages I2C, I2S, Codecs, and the RGB LED Strip.
 */
class Board : public HalBase {
public:
  static Board &getInstance();

  /**
   * @brief Initialize all board peripherals.
   * @return true if successful
   */
  bool begin() override;

  // Audio Configuration
  void setSampleRate(uint32_t sample_rate) { m_sample_rate = sample_rate; }
  void setInitialVolumes(int record, int play) {
    m_record_volume = record;
    m_play_volume = play;
  }

  // Hardware Handle Accessors
  i2c_master_bus_handle_t getI2cBus() { return m_i2c_bus; }
  i2s_chan_handle_t getTxHandle() { return m_tx_handle; }
  i2s_chan_handle_t getRxHandle() { return m_rx_handle; }
  esp_codec_dev_handle_t getPlayDev() { return m_play_dev; }
  esp_codec_dev_handle_t getRecordDev() { return m_record_dev; }
  esp_io_expander_handle_t getIoExpander() { return m_io_expander; }

  // Low-level LED Control (Driver Layer)
  void setLedPixel(uint32_t index, uint32_t r, uint32_t g, uint32_t b);
  void setAllLedsColor(uint32_t r, uint32_t g, uint32_t b);
  void refreshLeds();
  void clearLeds();

  // Audio Control
  esp_err_t setPlayVolume(int volume);
  esp_err_t getPlayVolume(int *volume);
  esp_err_t setRecordGain(float db_value);
  struct AudioFrame {
    int16_t mic;
    int16_t ref;
  };
  esp_err_t getAecFrames(AudioFrame *frames, int num_frames);
  esp_err_t getFeedData(int16_t *buffer, int buffer_len);

  // SD Card
  esp_err_t initSdCard(const char *mount_point, size_t max_files);

private:
  Board() = default;
  ~Board() = default;

  uint32_t m_sample_rate = 16000;
  int m_record_volume = 70;
  int m_play_volume = 80;

  i2c_master_bus_handle_t m_i2c_bus = nullptr;
  i2s_chan_handle_t m_tx_handle = nullptr;
  i2s_chan_handle_t m_rx_handle = nullptr;
  esp_codec_dev_handle_t m_play_dev = nullptr;
  esp_codec_dev_handle_t m_record_dev = nullptr;
  esp_io_expander_handle_t m_io_expander = nullptr;
  led_strip_handle_t m_led_strip = nullptr;
  int16_t *m_tdm_work_buffer = nullptr;
  static constexpr size_t TDM_BUF_SIZE = 1024 * 4;

  esp_err_t initI2c();
  esp_err_t initIoExpander();
  esp_err_t initI2s(uint32_t sample_rate);
  esp_err_t initCodecs(uint32_t sample_rate);
  void initRgbLeds();

  static constexpr const char *TAG = "Board";
};
