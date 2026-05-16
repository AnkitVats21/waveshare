#include "hal/audio/AudioHal.h"
#include "driver/i2s_tdm.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "hal/Board_defs.h"
#include "hal/i2s_types.h"
#include <string.h>

// ============================================================================
// Lifecycle
// ============================================================================

esp_err_t AudioHal::init(const Config &cfg) {
  m_i2c_bus       = cfg.i2c_bus;
  m_sample_rate   = cfg.sample_rate;
  m_record_volume = cfg.record_volume;
  m_play_volume   = cfg.play_volume;

  // Allocate SPIRAM work buffer for 4-channel TDM AEC reads
  m_tdm_work_buffer = (int16_t *)heap_caps_malloc(
      TDM_BUF_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!m_tdm_work_buffer) {
    ESP_LOGE(TAG, "Failed to allocate TDM work buffer");
    return ESP_ERR_NO_MEM;
  }

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
  ESP_LOGI(TAG, "AudioHal initialized at %lu Hz", m_sample_rate);
  return ESP_OK;
}

esp_err_t AudioHal::deinit() {
  if (!m_initialized) {
    ESP_LOGI(TAG, "Not initialized, nothing to deinit");
    return ESP_OK;
  }

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

  // Note: TDM work buffer is kept across reinit cycles to avoid re-alloc
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
// Private: I2S initialization
// ============================================================================

esp_err_t AudioHal::initI2s(uint32_t sample_rate) {
  // 1. Master channel config
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;

  esp_err_t ret = i2s_new_channel(&chan_cfg, &m_tx_handle, &m_rx_handle);
  if (ret != ESP_OK)
    return ret;

  // 2. Unified TDM clock
  i2s_tdm_clk_config_t unified_clk = I2S_TDM_CLK_DEFAULT_CONFIG(sample_rate);
  unified_clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  // 3. Shared GPIO mapping
  i2s_tdm_gpio_config_t shared_gpio = {
      .mclk = (gpio_num_t)GPIO_I2S_MCLK,
      .bclk = (gpio_num_t)GPIO_I2S_SCLK,
      .ws   = (gpio_num_t)GPIO_I2S_LRCK,
      .dout = (gpio_num_t)GPIO_I2S_DOUT,
      .din  = (gpio_num_t)GPIO_I2S_SDIN,
      .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
  };

  // 4. RX: Mono, Slot 0 only, 2 slots on wire for clock symmetry
  i2s_tdm_config_t rx_tdm_cfg = {
      .clk_cfg  = unified_clk,
      .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO,
          (i2s_tdm_slot_mask_t)I2S_TDM_SLOT0),
      .gpio_cfg = shared_gpio,
  };
  rx_tdm_cfg.slot_cfg.total_slot = I2S_TDM_SLOT2;

  // 5. TX: Mono, Slot 0 only, match RX geometry exactly
  i2s_tdm_config_t tx_tdm_cfg = {
      .clk_cfg  = unified_clk,
      .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO,
          (i2s_tdm_slot_mask_t)I2S_TDM_SLOT0),
      .gpio_cfg = shared_gpio,
  };
  tx_tdm_cfg.slot_cfg.total_slot = I2S_TDM_SLOT2;

  ret |= i2s_channel_init_tdm_mode(m_tx_handle, &tx_tdm_cfg);
  ret |= i2s_channel_init_tdm_mode(m_rx_handle, &rx_tdm_cfg);
  ret |= i2s_channel_enable(m_tx_handle);
  ret |= i2s_channel_enable(m_rx_handle);

  return ret;
}

// ============================================================================
// Private: Codec initialization (ES7210 ADC + ES8311 DAC)
// ============================================================================

esp_err_t AudioHal::initCodecs(uint32_t sample_rate) {
  // --- Record path: ES7210 ADC ---
  audio_codec_i2s_cfg_t i2s_cfg = {};
  i2s_cfg.port      = I2S_NUM_1;
  i2s_cfg.rx_handle = m_rx_handle;
  i2s_cfg.tx_handle = nullptr;
  const audio_codec_data_if_t *record_data_if =
      audio_codec_new_i2s_data(&i2s_cfg);

  audio_codec_i2c_cfg_t i2c_cfg_adc = {};
  i2c_cfg_adc.addr       = ES7210_CODEC_DEFAULT_ADDR;
  i2c_cfg_adc.bus_handle = m_i2c_bus;
  const audio_codec_ctrl_if_t *record_ctrl_if =
      audio_codec_new_i2c_ctrl(&i2c_cfg_adc);

  es7210_codec_cfg_t es7210_cfg = {};
  es7210_cfg.ctrl_if      = record_ctrl_if;
  es7210_cfg.mic_selected = ES7210_SEL_MIC1; // Only Mic 1 for Mono record
  es7210_cfg.master_mode  = false;
  es7210_cfg.mclk_src     = ES7210_MCLK_FROM_PAD;
  es7210_cfg.mclk_div     = 0;
  const audio_codec_if_t *record_codec_if = es7210_codec_new(&es7210_cfg);

  esp_codec_dev_cfg_t record_dev_cfg = {};
  record_dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
  record_dev_cfg.codec_if = record_codec_if;
  record_dev_cfg.data_if  = record_data_if;
  m_record_dev = esp_codec_dev_new(&record_dev_cfg);

  // --- Playback path: ES8311 DAC ---
  audio_codec_i2s_cfg_t i2s_cfg_tx = {};
  i2s_cfg_tx.port      = I2S_NUM_1;
  i2s_cfg_tx.rx_handle = nullptr;
  i2s_cfg_tx.tx_handle = m_tx_handle;
  const audio_codec_data_if_t *play_data_if =
      audio_codec_new_i2s_data(&i2s_cfg_tx);

  audio_codec_i2c_cfg_t i2c_cfg_dac = {};
  i2c_cfg_dac.addr       = ES8311_CODEC_DEFAULT_ADDR;
  i2c_cfg_dac.bus_handle = m_i2c_bus;
  const audio_codec_ctrl_if_t *play_ctrl_if =
      audio_codec_new_i2c_ctrl(&i2c_cfg_dac);
  const audio_codec_gpio_if_t *play_gpio_if = audio_codec_new_gpio();

  es8311_codec_cfg_t es8311_cfg = {};
  es8311_cfg.ctrl_if     = play_ctrl_if;
  es8311_cfg.gpio_if     = play_gpio_if;
  es8311_cfg.codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC;
  es8311_cfg.pa_pin      = GPIO_PWR_CTRL;
  es8311_cfg.pa_reverted = false;
  es8311_cfg.master_mode = false;
  es8311_cfg.use_mclk    = false;
  es8311_cfg.digital_mic = false;
  es8311_cfg.invert_mclk = false;
  es8311_cfg.invert_sclk = false;
  es8311_cfg.hw_gain     = {};
  es8311_cfg.no_dac_ref  = false;
  es8311_cfg.mclk_div    = 0;
  const audio_codec_if_t *play_codec_if = es8311_codec_new(&es8311_cfg);

  esp_codec_dev_cfg_t play_dev_cfg = {};
  play_dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
  play_dev_cfg.codec_if = play_codec_if;
  play_dev_cfg.data_if  = play_data_if;
  m_play_dev = esp_codec_dev_new(&play_dev_cfg);

  // Unified sample format: 1-channel Mono, 16-bit
  esp_codec_dev_sample_info_t fs = {};
  fs.sample_rate    = sample_rate;
  fs.channel        = 1;
  fs.bits_per_sample = 16;
  fs.channel_mask   = 0;
  fs.mclk_multiple  = 0;

  esp_codec_dev_open(m_record_dev, &fs);
  esp_codec_dev_set_in_channel_gain(
      m_record_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), (float)m_record_volume);

  esp_codec_dev_set_out_vol(m_play_dev, m_play_volume);
  esp_codec_dev_open(m_play_dev, &fs);

  return ESP_OK;
}

// ============================================================================
// Audio frame APIs
// ============================================================================

esp_err_t AudioHal::getFeedData(int16_t *buffer, int buffer_len) {
  if (!m_initialized || !m_record_dev)
    return ESP_FAIL;

  // Mono: 1 channel * 2 bytes per sample
  int bytes_to_read = buffer_len * 1 * sizeof(int16_t);
  return (esp_codec_dev_read(m_record_dev, (void *)buffer, bytes_to_read) ==
          ESP_OK)
             ? ESP_OK
             : ESP_FAIL;
}

esp_err_t AudioHal::getAecFrames(AecFrame *frames, int num_frames) {
  if (!m_initialized || !m_record_dev || !m_tdm_work_buffer)
    return ESP_FAIL;

  // 4 TDM channels * 2 bytes per sample
  size_t bytes_to_read = num_frames * 4 * sizeof(int16_t);
  if (bytes_to_read > TDM_BUF_SIZE * sizeof(int16_t))
    bytes_to_read = TDM_BUF_SIZE * sizeof(int16_t);

  esp_err_t ret = esp_codec_dev_read(m_record_dev, (void *)m_tdm_work_buffer,
                                     bytes_to_read);
  if (ret == ESP_OK) {
    int actual_samples = bytes_to_read / (sizeof(int16_t) * 4);
    for (int i = 0; i < actual_samples; i++) {
      frames[i].mic = m_tdm_work_buffer[i * 4 + 0]; // Slot 0: Mic 1
      frames[i].ref = m_tdm_work_buffer[i * 4 + 3]; // Slot 3: Speaker ref
    }
    return ESP_OK;
  }
  return ESP_FAIL;
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
