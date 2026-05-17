#include "hal/audio/AudioHal.h"
#include "driver/i2s_std.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "hal/Board_defs.h"
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Lifecycle
// ============================================================================

esp_err_t AudioHal::init(const Config &cfg) {
  m_i2c_bus       = cfg.i2c_bus;
  m_sample_rate   = cfg.sample_rate;
  m_record_volume = cfg.record_volume;
  m_play_volume   = cfg.play_volume;

  esp_err_t ret = initI2s(m_sample_rate);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = initCodecs(m_sample_rate);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Codec init failed: %s", esp_err_to_name(ret));
    return ret;
  }

  m_initialized = true;
  ESP_LOGI(TAG, "AudioHal initialized at %lu Hz (STD I2S, 4-ch ES7210)", m_sample_rate);
  return ESP_OK;
}

esp_err_t AudioHal::deinit() {
  if (!m_initialized) {
    ESP_LOGI(TAG, "Not initialized, nothing to deinit");
    return ESP_OK;
  }

  // 1. Close + delete codec devices first
  if (m_record_dev) {
    esp_codec_dev_close(m_record_dev);
    esp_codec_dev_delete(m_record_dev);
    m_record_dev = nullptr;
  }
  if (m_play_dev) {
    esp_codec_dev_close(m_play_dev);
    esp_codec_dev_delete(m_play_dev);
    m_play_dev = nullptr;
  }

  // 2. Delete codec interfaces (MUST happen before i2s_del_channel so the
  //    esp_codec_dev framework releases its internal reference to the handles)
  if (m_record_codec_if) { audio_codec_delete_codec_if(m_record_codec_if); m_record_codec_if = nullptr; }
  if (m_record_ctrl_if)  { audio_codec_delete_ctrl_if(m_record_ctrl_if);   m_record_ctrl_if  = nullptr; }
  if (m_record_data_if)  { audio_codec_delete_data_if(m_record_data_if);   m_record_data_if  = nullptr; }
  if (m_play_codec_if)   { audio_codec_delete_codec_if(m_play_codec_if);   m_play_codec_if   = nullptr; }
  if (m_play_gpio_if)    { audio_codec_delete_gpio_if(m_play_gpio_if);     m_play_gpio_if    = nullptr; }
  if (m_play_ctrl_if)    { audio_codec_delete_ctrl_if(m_play_ctrl_if);     m_play_ctrl_if    = nullptr; }
  if (m_play_data_if)    { audio_codec_delete_data_if(m_play_data_if);     m_play_data_if    = nullptr; }

  // 3. Disable + delete I2S channels
  if (m_tx_handle) {
    i2s_channel_disable(m_tx_handle);
    i2s_del_channel(m_tx_handle);
    m_tx_handle = nullptr;
  }
  if (m_rx_handle) {
    i2s_channel_disable(m_rx_handle);
    i2s_del_channel(m_rx_handle);
    m_rx_handle = nullptr;
  }

  m_initialized = false;
  ESP_LOGI(TAG, "Audio deinitialized");
  return ESP_OK;
}

esp_err_t AudioHal::reinit(uint32_t sample_rate) {
  esp_err_t res = deinit();
  if (res != ESP_OK) {
    ESP_LOGE(TAG, "Failed to deinit before reinit");
    return res;
  }

  m_sample_rate = sample_rate;

  res = initI2s(sample_rate);
  if (res != ESP_OK) {
    ESP_LOGE(TAG, "Failed to reinit I2S");
    return res;
  }

  res = initCodecs(sample_rate);
  if (res != ESP_OK) {
    ESP_LOGE(TAG, "Failed to reinit Codecs");
    return res;
  }

  m_initialized = true;
  ESP_LOGI(TAG, "AudioHal reinitialized at %lu Hz", sample_rate);
  return res;
}

// ============================================================================
// Private: I2S STD initialization (matches verified Waveshare demo BSP)
//
// The ES7210 and ES8311 share a single I2S_STD bus on I2S_NUM_1.
// Both codecs are clocked together in STEREO 32-bit mode.  The ES7210
// delivers 4 channels by time-multiplexing its 4 mics into the two 32-bit
// stereo slots — two mics per WS half-cycle.
// ============================================================================

esp_err_t AudioHal::initI2s(uint32_t sample_rate) {
  // 1. Master channel pair
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;

  esp_err_t ret = i2s_new_channel(&chan_cfg, &m_tx_handle, &m_rx_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
    return ret;
  }

  // 2. STD Philips mode — Stereo, 32-bit (ES7210 needs ≥32-bit frames)
  i2s_std_config_t std_cfg = {
      .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = (gpio_num_t)GPIO_I2S_MCLK,
          .bclk = (gpio_num_t)GPIO_I2S_SCLK,
          .ws   = (gpio_num_t)GPIO_I2S_LRCK,
          .dout = (gpio_num_t)GPIO_I2S_DOUT,
          .din  = (gpio_num_t)GPIO_I2S_SDIN,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv   = false,
          },
      },
  };

  ret |= i2s_channel_init_std_mode(m_tx_handle, &std_cfg);
  ret |= i2s_channel_init_std_mode(m_rx_handle, &std_cfg);
  ret |= i2s_channel_enable(m_tx_handle);
  ret |= i2s_channel_enable(m_rx_handle);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "I2S STD channel init/enable failed: 0x%x", ret);
  }
  return ret;
}

// ============================================================================
// Private: Codec initialization — ES7210 ADC (4 mics) + ES8311 DAC
// ============================================================================

esp_err_t AudioHal::initCodecs(uint32_t sample_rate) {
  // ---- Record path: ES7210 ADC (4-channel) ----------------------------------
  audio_codec_i2s_cfg_t i2s_cfg_rx = {};
  i2s_cfg_rx.port      = I2S_NUM_1;
  i2s_cfg_rx.rx_handle = m_rx_handle;
  i2s_cfg_rx.tx_handle = nullptr;
  m_record_data_if = audio_codec_new_i2s_data(&i2s_cfg_rx);

  audio_codec_i2c_cfg_t i2c_cfg_adc = {};
  i2c_cfg_adc.addr       = ES7210_CODEC_DEFAULT_ADDR;
  i2c_cfg_adc.bus_handle = m_i2c_bus;
  m_record_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg_adc);

  es7210_codec_cfg_t es7210_cfg = {};
  es7210_cfg.ctrl_if      = m_record_ctrl_if;
  es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 |
                            ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
  m_record_codec_if = es7210_codec_new(&es7210_cfg);

  esp_codec_dev_cfg_t record_dev_cfg = {};
  record_dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
  record_dev_cfg.codec_if = m_record_codec_if;
  record_dev_cfg.data_if  = m_record_data_if;
  m_record_dev = esp_codec_dev_new(&record_dev_cfg);

  esp_codec_dev_sample_info_t record_fs = {};
  record_fs.sample_rate    = sample_rate;
  record_fs.channel        = 2;
  record_fs.bits_per_sample = 32;
  esp_codec_dev_open(m_record_dev, &record_fs);

  esp_codec_dev_set_in_channel_gain(
      m_record_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), (float)m_record_volume);
  esp_codec_dev_set_in_channel_gain(
      m_record_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1), (float)m_record_volume);
  esp_codec_dev_set_in_channel_gain(
      m_record_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2), (float)m_record_volume);
  esp_codec_dev_set_in_channel_gain(
      m_record_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3), (float)m_record_volume);

  // ---- Playback path: ES8311 DAC -------------------------------------------
  audio_codec_i2s_cfg_t i2s_cfg_tx = {};
  i2s_cfg_tx.port      = I2S_NUM_1;
  i2s_cfg_tx.rx_handle = nullptr;
  i2s_cfg_tx.tx_handle = m_tx_handle;
  m_play_data_if = audio_codec_new_i2s_data(&i2s_cfg_tx);

  audio_codec_i2c_cfg_t i2c_cfg_dac = {};
  i2c_cfg_dac.addr       = ES8311_CODEC_DEFAULT_ADDR;
  i2c_cfg_dac.bus_handle = m_i2c_bus;
  m_play_ctrl_if  = audio_codec_new_i2c_ctrl(&i2c_cfg_dac);
  m_play_gpio_if  = audio_codec_new_gpio();

  es8311_codec_cfg_t es8311_cfg = {};
  es8311_cfg.ctrl_if     = m_play_ctrl_if;
  es8311_cfg.gpio_if     = m_play_gpio_if;
  es8311_cfg.codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC;
  es8311_cfg.pa_pin      = GPIO_PWR_CTRL;
  es8311_cfg.pa_reverted = false;
  es8311_cfg.master_mode = false;
  es8311_cfg.use_mclk    = false;
  m_play_codec_if = es8311_codec_new(&es8311_cfg);

  esp_codec_dev_cfg_t play_dev_cfg = {};
  play_dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
  play_dev_cfg.codec_if = m_play_codec_if;
  play_dev_cfg.data_if  = m_play_data_if;
  m_play_dev = esp_codec_dev_new(&play_dev_cfg);

  esp_codec_dev_sample_info_t play_fs = {};
  play_fs.sample_rate    = sample_rate;
  play_fs.channel        = 2;
  play_fs.bits_per_sample = 32;
  esp_codec_dev_set_out_vol(m_play_dev, m_play_volume);
  esp_codec_dev_open(m_play_dev, &play_fs);

  return ESP_OK;
}

// ============================================================================
// Audio frame APIs
// ============================================================================

/**
 * Read 4-channel interleaved audio from the ES7210 ADC.
 *
 * is_get_raw_channel = true  → buffer receives 4 raw int16 channels/frame
 *                              (layout: slot0, slot1, slot2, slot3 — i.e. RMNM)
 * is_get_raw_channel = false → remap to 3-channel: [Mic1, Mic2, Ref]
 *
 * The hardware read is always done as 4 * int16 per frame regardless.
 * The ES7210 is opened in 32-bit stereo; each 32-bit word holds two 16-bit
 * mic samples.  esp_codec_dev_read returns them already as int16 pairs.
 */
esp_err_t AudioHal::getFeedData(bool is_get_raw_channel, int16_t *buffer,
                                int buffer_len) {
  if (!m_initialized || !m_record_dev)
    return ESP_FAIL;

  esp_err_t ret = esp_codec_dev_read(m_record_dev, (void *)buffer, buffer_len);

  if (ret == ESP_OK && !is_get_raw_channel) {
    // Remap from 4-ch raw to 3-ch [Mic1, Mic2, Ref]
    // Raw layout per frame: [Ref(0), Mic1(1), Unused(2), Mic2(3)]  ← RMNM
    int audio_chunksize = buffer_len / (sizeof(int16_t) * ADC_I2S_CHANNEL);
    for (int i = 0; i < audio_chunksize; i++) {
      int16_t ref       = buffer[4 * i + 0];
      buffer[3 * i + 0] = buffer[4 * i + 1]; // Mic1
      buffer[3 * i + 1] = buffer[4 * i + 3]; // Mic2
      buffer[3 * i + 2] = ref;               // Ref (for AEC)
    }
  }

  return ret;
}

/**
 * Convenience overload — raw 4-channel, for MicCapture streaming pipeline.
 * After this call the caller typically uses only channel 0 (primary mic).
 */
esp_err_t AudioHal::getFeedData(int16_t *buffer, int buffer_len) {
  return getFeedData(true, buffer, buffer_len);
}

// ============================================================================
// Playback
// ============================================================================

esp_err_t AudioHal::audioPlay(const int16_t *data, int length,
                              uint32_t /*ticks_to_wait*/) {
  if (!m_initialized || !m_play_dev)
    return ESP_FAIL;

  // The I2S bus runs in stereo 32-bit mode.  Incoming data is 16-bit mono.
  // Expand: each int16 → int32 (shift left 16 bits), then duplicate for stereo.
  int num_samples = length / sizeof(int16_t);
  // 2 stereo channels * 2 bytes (32-bit expanded from 16-bit)
  int out_len = num_samples * 2 * sizeof(int32_t);
  int32_t *out_buf = (int32_t *)malloc(out_len);
  if (!out_buf) return ESP_ERR_NO_MEM;

  for (int i = 0; i < num_samples; i++) {
    int32_t sample = ((int32_t)data[i]) << 16;
    out_buf[2 * i + 0] = sample; // L
    out_buf[2 * i + 1] = sample; // R
  }

  esp_err_t ret = esp_codec_dev_write(m_play_dev, out_buf, out_len);
  free(out_buf);
  return ret;
}

// ============================================================================
// Volume / Gain control
// ============================================================================

esp_err_t AudioHal::setPlayVolume(int volume) {
  if (!m_initialized || !m_play_dev)
    return ESP_FAIL;
  m_play_volume = volume;
  return esp_codec_dev_set_out_vol(m_play_dev, volume);
}

esp_err_t AudioHal::getPlayVolume(int *volume) {
  if (!m_initialized || !m_play_dev)
    return ESP_FAIL;
  return esp_codec_dev_get_out_vol(m_play_dev, volume);
}

esp_err_t AudioHal::setRecordGain(float db_value) {
  if (!m_initialized || !m_record_dev)
    return ESP_FAIL;
  m_record_volume = (int)db_value;
  return esp_codec_dev_set_in_gain(m_record_dev, db_value);
}
