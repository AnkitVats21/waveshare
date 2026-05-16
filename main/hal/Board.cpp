#include "Board.h"
#include "driver/gpio.h"
#include "driver/i2s_tdm.h"
#include "driver/sdmmc_host.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/i2s_types.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"
#include <string.h>

/*
WAKE WORD to be implemented along with AEC and VAD in the future. For now, we
will just capture raw audio data from the ADC and provide it to the application
layer for processing. The getAecFrames() method will return interleaved frames
containing both the microphone signal and the reference signal for AEC, allowing
the application to implement its own AEC algorithm or pass the data to
a third-party library for processing. This approach keeps the Board classfocused
on hardware interfacing, while giving maximum flexibility to the application
layer for audio processing and wake word detection.
*/

Board &Board::getInstance() {
  static Board instance;
  return instance;
}

struct AudioFrame {
  int16_t mic; // Primary Voice
  int16_t ref; // Speaker Reference for AEC
};

bool Board::begin() {
  if (m_initialized)
    return true;

  m_tdm_work_buffer = (int16_t *)heap_caps_malloc(
      TDM_BUF_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);

  ESP_LOGI(TAG, "Initializing Board Hardware at %lu Hz...", m_sample_rate);

  // 0. Initialize NVS
  esp_err_t nvs_ret = nvs_flash_init();
  if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_ret = nvs_flash_init();
  }
  if (nvs_ret != ESP_OK)
    return false;

  // 1. Initialize I2C Bus
  if (initI2c() != ESP_OK)
    return false;

  // 2. Initialize IO Expander
  if (initIoExpander() != ESP_OK)
    return false;

  // 3. Initialize I2S
  if (initI2s(m_sample_rate) != ESP_OK)
    return false;

  // 4. Initialize Codecs
  if (initCodecs(m_sample_rate) != ESP_OK)
    return false;

  // 5. Initialize RGB LEDs (Directly in Board)
  initRgbLeds();

  m_initialized = true;
  return true;
}

esp_err_t Board::initI2c() {
  if (m_i2c_bus != nullptr)
    return ESP_OK;

  i2c_master_bus_config_t bus_config = {};
  bus_config.i2c_port = I2C_NUM;
  bus_config.sda_io_num = (gpio_num_t)GPIO_I2C_SDA;
  bus_config.scl_io_num = (gpio_num_t)GPIO_I2C_SCL;
  bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_config.glitch_ignore_cnt = 7;
  bus_config.intr_priority = 0;
  bus_config.trans_queue_depth = 0;
  bus_config.flags.enable_internal_pullup = true;

  gpio_set_pull_mode(bus_config.sda_io_num, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(bus_config.scl_io_num, GPIO_PULLUP_ONLY);

  esp_err_t ret = i2c_new_master_bus(&bus_config, &m_i2c_bus);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(ret));
    return ret;
  }
  return ESP_OK;
}

esp_err_t Board::initIoExpander() {
  esp_err_t ret = esp_io_expander_new_i2c_tca95xx_16bit(
      m_i2c_bus, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000, &m_io_expander);

  if (ret == ESP_OK) {
    esp_io_expander_set_dir(m_io_expander,
                            IO_EXPANDER_PIN_NUM_8 | IO_EXPANDER_PIN_NUM_9 |
                                IO_EXPANDER_PIN_NUM_10,
                            IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(m_io_expander,
                              IO_EXPANDER_PIN_NUM_8 | IO_EXPANDER_PIN_NUM_9 |
                                  IO_EXPANDER_PIN_NUM_10,
                              1);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  return ret;
}

esp_err_t Board::initI2s(uint32_t sample_rate) {
  // 1. Configure the Master Controller Properties
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;

  esp_err_t ret = i2s_new_channel(&chan_cfg, &m_tx_handle, &m_rx_handle);
  if (ret != ESP_OK)
    return ret;

  // 2. Set up a Unified TDM Clock Configuration
  i2s_tdm_clk_config_t unified_clk = I2S_TDM_CLK_DEFAULT_CONFIG(sample_rate);
  unified_clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  // 3. Define the shared GPIO pin mappings across the audio bus
  i2s_tdm_gpio_config_t shared_gpio = {
      .mclk = (gpio_num_t)GPIO_I2S_MCLK,
      .bclk = (gpio_num_t)GPIO_I2S_SCLK,
      .ws = (gpio_num_t)GPIO_I2S_LRCK,
      .dout = (gpio_num_t)GPIO_I2S_DOUT,
      .din = (gpio_num_t)GPIO_I2S_SDIN,
      .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
  };

  // 4. RX Configuration: Switched to MONO mode, masking ONLY Slot 0
  i2s_tdm_config_t rx_tdm_cfg = {
      .clk_cfg = unified_clk,
      .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_16BIT,
          I2S_SLOT_MODE_MONO,                  // Forced to Mono
          (i2s_tdm_slot_mask_t)I2S_TDM_SLOT0), // Only Slot 0 active
      .gpio_cfg = shared_gpio,
  };
  rx_tdm_cfg.slot_cfg.total_slot =
      I2S_TDM_SLOT2; // 2 slots on wire for clock symmetry

  // 5. TX Configuration: Switched to MONO mode, masking ONLY Slot 0
  i2s_tdm_config_t tx_tdm_cfg = {
      .clk_cfg = unified_clk,
      .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_16BIT,
          I2S_SLOT_MODE_MONO,                  // Forced to Mono
          (i2s_tdm_slot_mask_t)I2S_TDM_SLOT0), // Only Slot 0 active
      .gpio_cfg = shared_gpio,
  };
  tx_tdm_cfg.slot_cfg.total_slot = I2S_TDM_SLOT2; // Match geometry exactly

  // Initialize BOTH channels using TDM mode
  ret |= i2s_channel_init_tdm_mode(m_tx_handle, &tx_tdm_cfg);
  ret |= i2s_channel_init_tdm_mode(m_rx_handle, &rx_tdm_cfg);

  // Enable the hardware lines simultaneously
  ret |= i2s_channel_enable(m_tx_handle);
  ret |= i2s_channel_enable(m_rx_handle);

  return ret;
}

esp_err_t Board::initCodecs(uint32_t sample_rate) {
  audio_codec_i2s_cfg_t i2s_cfg = {};
  i2s_cfg.port = I2S_NUM_1;
  i2s_cfg.rx_handle = m_rx_handle;
  i2s_cfg.tx_handle = nullptr;
  const audio_codec_data_if_t *record_data_if =
      audio_codec_new_i2s_data(&i2s_cfg);

  audio_codec_i2c_cfg_t i2c_cfg_adc = {};
  i2c_cfg_adc.addr = ES7210_CODEC_DEFAULT_ADDR;
  i2c_cfg_adc.bus_handle = m_i2c_bus;
  const audio_codec_ctrl_if_t *record_ctrl_if =
      audio_codec_new_i2c_ctrl(&i2c_cfg_adc);

  es7210_codec_cfg_t es7210_cfg = {};
  es7210_cfg.ctrl_if = record_ctrl_if;
  es7210_cfg.mic_selected =
      ES7210_SEL_MIC1; // MODIFIED: Only select Mic 1 for Mono record
  es7210_cfg.master_mode = false;
  es7210_cfg.mclk_src = ES7210_MCLK_FROM_PAD;
  es7210_cfg.mclk_div = 0;
  const audio_codec_if_t *record_codec_if = es7210_codec_new(&es7210_cfg);

  esp_codec_dev_cfg_t record_dev_cfg = {};
  record_dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
  record_dev_cfg.codec_if = record_codec_if;
  record_dev_cfg.data_if = record_data_if;
  m_record_dev = esp_codec_dev_new(&record_dev_cfg);

  audio_codec_i2s_cfg_t i2s_cfg_tx = {};
  i2s_cfg_tx.port = I2S_NUM_1;
  i2s_cfg_tx.rx_handle = nullptr;
  i2s_cfg_tx.tx_handle = m_tx_handle;
  const audio_codec_data_if_t *play_data_if =
      audio_codec_new_i2s_data(&i2s_cfg_tx);

  audio_codec_i2c_cfg_t i2c_cfg_dac = {};
  i2c_cfg_dac.addr = ES8311_CODEC_DEFAULT_ADDR;
  i2c_cfg_dac.bus_handle = m_i2c_bus;
  const audio_codec_ctrl_if_t *play_ctrl_if =
      audio_codec_new_i2c_ctrl(&i2c_cfg_dac);
  const audio_codec_gpio_if_t *play_gpio_if = audio_codec_new_gpio();

  es8311_codec_cfg_t es8311_cfg = {};
  es8311_cfg.ctrl_if = play_ctrl_if;
  es8311_cfg.gpio_if = play_gpio_if;
  es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
  es8311_cfg.pa_pin = GPIO_PWR_CTRL;
  es8311_cfg.pa_reverted = false;
  es8311_cfg.master_mode = false;
  es8311_cfg.use_mclk = false;
  es8311_cfg.digital_mic = false;
  es8311_cfg.invert_mclk = false;
  es8311_cfg.invert_sclk = false;
  es8311_cfg.hw_gain = {};
  es8311_cfg.no_dac_ref = false;
  es8311_cfg.mclk_div = 0;
  const audio_codec_if_t *play_codec_if = es8311_codec_new(&es8311_cfg);

  esp_codec_dev_cfg_t play_dev_cfg = {};
  play_dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
  play_dev_cfg.codec_if = play_codec_if;
  play_dev_cfg.data_if = play_data_if;
  m_play_dev = esp_codec_dev_new(&play_dev_cfg);

  // Unified sample info structure forced explicitly to 1 channel (Mono)
  esp_codec_dev_sample_info_t fs = {};
  fs.sample_rate = sample_rate;
  fs.channel = 1; // MODIFIED: 1 Channel (Mono)
  fs.bits_per_sample = 16;
  fs.channel_mask = 0;
  fs.mclk_multiple = 0;

  // Open record pipeline and assign gain strictly to Channel 0 (Slot 0)
  esp_codec_dev_open(m_record_dev, &fs);
  esp_codec_dev_set_in_channel_gain(
      m_record_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), (float)m_record_volume);

  // Open playback pipeline
  esp_codec_dev_set_out_vol(m_play_dev, m_play_volume);
  esp_codec_dev_open(m_play_dev, &fs);

  return ESP_OK;
}

esp_err_t Board::getFeedData(int16_t *buffer, int buffer_len) {
  if (!m_record_dev)
    return ESP_FAIL;

  // Mono calculation: 1 channel * 2 bytes per sample = 2 bytes per sample point
  int bytes_to_read = buffer_len * 1 * sizeof(int16_t);

  // Directly read the data directly into your target application buffer
  if (esp_codec_dev_read(m_record_dev, (void *)buffer, bytes_to_read) ==
      ESP_OK) {
    return ESP_OK;
  }

  return ESP_FAIL;
}

esp_err_t Board::setPlayVolume(int volume) {
  if (!m_play_dev)
    return ESP_FAIL;
  m_play_volume = volume;
  return esp_codec_dev_set_out_vol(m_play_dev, volume);
}

esp_err_t Board::getPlayVolume(int *volume) {
  if (!m_play_dev)
    return ESP_FAIL;
  return esp_codec_dev_get_out_vol(m_play_dev, volume);
}

esp_err_t Board::setRecordGain(float db_value) {
  if (!m_record_dev)
    return ESP_FAIL;
  m_record_volume = (int)db_value;
  return esp_codec_dev_set_in_gain(m_record_dev, db_value);
}

esp_err_t Board::getAecFrames(AudioFrame *frames, int num_frames) {
  if (!m_record_dev || !m_tdm_work_buffer)
    return ESP_FAIL;

  // Calculate bytes: 4 channels * 2 bytes per sample * number of frames
  size_t bytes_to_read = num_frames * 4 * sizeof(int16_t);

  // Ensure we don't overflow the work buffer
  if (bytes_to_read > TDM_BUF_SIZE * sizeof(int16_t)) {
    bytes_to_read = TDM_BUF_SIZE * sizeof(int16_t);
  }

  esp_err_t ret = esp_codec_dev_read(m_record_dev, (void *)m_tdm_work_buffer,
                                     bytes_to_read);

  if (ret == ESP_OK) {
    int actual_samples = bytes_to_read / (sizeof(int16_t) * 4);
    for (int i = 0; i < actual_samples; i++) {
      /**
       * Channel Mapping for Waveshare ESP32-S3 Audio:
       * Slot 0: Mic 1
       * Slot 1: Mic 2
       * Slot 3: Speaker Loopback (Reference)
       */

      // Use Mic 1 as primary voice signal
      frames[i].mic = m_tdm_work_buffer[i * 4 + 0];

      // Use Slot 3 as AEC Reference
      frames[i].ref = m_tdm_work_buffer[i * 4 + 3];

      // OPTIONAL: Basic Software AEC (Simple subtraction)
      // For real AEC, pass frames[i].mic and frames[i].ref to an Adaptive
      // Filter frames[i].mic = frames[i].mic - (frames[i].ref * gain);
    }
    return ESP_OK;
  }
  return ESP_FAIL;
}

esp_err_t Board::initSdCard(const char *mount_point, size_t max_files) {
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
  mount_config.format_if_mount_failed = false;
  mount_config.max_files = (int)max_files;
  mount_config.allocation_unit_size = 16 * 1024;
  mount_config.disk_status_check_enable = false;

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = SDMMC_BUS_WIDTH;
  slot_config.clk = (gpio_num_t)GPIO_SDMMC_CLK;
  slot_config.cmd = (gpio_num_t)GPIO_SDMMC_CMD;
  slot_config.d0 = (gpio_num_t)GPIO_SDMMC_D0;
  slot_config.d1 = (gpio_num_t)GPIO_SDMMC_D1;
  slot_config.d2 = (gpio_num_t)GPIO_SDMMC_D2;
  slot_config.d3 = (gpio_num_t)GPIO_SDMMC_D3;
  slot_config.cd = (gpio_num_t)GPIO_SDMMC_DET;
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  sdmmc_card_t *card;
  esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config,
                                          &mount_config, &card);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
    return ret;
  }
  sdmmc_card_print_info(stdout, card);
  return ESP_OK;
}

void Board::initRgbLeds() {
  led_strip_config_t strip_config = {};
  strip_config.strip_gpio_num = LED_STRIP_GPIO_PIN;
  strip_config.max_leds = LED_STRIP_LED_COUNT;
  strip_config.led_pixel_format = LED_PIXEL_FORMAT_GRB;
  strip_config.led_model = LED_MODEL_WS2812;
  strip_config.flags.invert_out = false;

  led_strip_rmt_config_t rmt_config = {};
  rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
  rmt_config.resolution_hz = 10 * 1000 * 1000; // 10MHz
  rmt_config.flags.with_dma = false;

  esp_err_t ret =
      led_strip_new_rmt_device(&strip_config, &rmt_config, &m_led_strip);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize LED strip: %s", esp_err_to_name(ret));
    return;
  }

  led_strip_clear(m_led_strip);
  ESP_LOGI(TAG, "RGB LED Strip initialized on GPIO %d", LED_STRIP_GPIO_PIN);
}

void Board::setLedPixel(uint32_t index, uint32_t r, uint32_t g, uint32_t b) {
  if (m_led_strip) {
    led_strip_set_pixel(m_led_strip, index, r, g, b);
  }
}

void Board::setAllLedsColor(uint32_t r, uint32_t g, uint32_t b) {
  if (m_led_strip) {
    for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
      led_strip_set_pixel(m_led_strip, i, r, g, b);
    }
    led_strip_refresh(m_led_strip);
  }
}

void Board::refreshLeds() {
  if (m_led_strip) {
    led_strip_refresh(m_led_strip);
  }
}

void Board::clearLeds() {
  if (m_led_strip) {
    led_strip_clear(m_led_strip);
    led_strip_refresh(m_led_strip);
  }
}
