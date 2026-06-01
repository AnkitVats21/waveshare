#include "Board.h"
#include "driver/gpio.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

/**
 * Board is the hardware orchestration layer.
 *
 * After Phase 2 refactor, Board::begin() purely coordinates initialization
 * of composed HAL components in the correct dependency order:
 *
 *   I2CBus → IoExpander → AudioHal → LedStripManager
 *
 * All driver-level implementation has been delegated to the respective
 * HAL component classes. Board contains only orchestration logic.
 */

Board &Board::getInstance() {
  static Board instance;
  return instance;
}

// ---------------------------------------------------------------------------
// Board boot sequence — hardware orchestration only
// ---------------------------------------------------------------------------

bool Board::begin() {
  if (m_initialized)
    return true;

  ESP_LOGI(TAG, "Initializing Board Hardware at %lu Hz...", m_sample_rate);

  // 0. NVS flash (required before WiFi and other subsystems)
  esp_err_t nvs_ret = nvs_flash_init();
  if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_ret = nvs_flash_init();
  }
  if (nvs_ret != ESP_OK)
    return false;

  // 1. I2C bus — prerequisite for IO expander and codec control
  if (m_i2c.init(I2C_NUM, (gpio_num_t)GPIO_I2C_SDA,
                 (gpio_num_t)GPIO_I2C_SCL) != ESP_OK)
    return false;

  // 2. IO Expander — must power up codec rails before I2S/codec init
  if (m_io.init(m_i2c.getBusHandle(),
                ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000) != ESP_OK)
    return false;

  // Power-on PA enable and peripheral power rails via IO expander pins 0, 1, 5, 6, 8
  m_io.setDirection(IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 |
                    IO_EXPANDER_PIN_NUM_5 | IO_EXPANDER_PIN_NUM_6 |
                    IO_EXPANDER_PIN_NUM_8,
                    true /* output */);
  esp_io_expander_set_level(m_io.getRawHandle(),
                            IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 |
                            IO_EXPANDER_PIN_NUM_5 | IO_EXPANDER_PIN_NUM_6 |
                            IO_EXPANDER_PIN_NUM_8,
                            1);
  vTaskDelay(pdMS_TO_TICKS(200)); // Allow power rails to stabilize

  // 3. Audio HAL — I2S + ES7210/ES8311 codec initialization
  AudioHal::Config audio_cfg;
  audio_cfg.sample_rate   = m_sample_rate;
  audio_cfg.record_volume = m_record_volume;
  audio_cfg.play_volume   = m_play_volume;
  audio_cfg.i2c_bus       = m_i2c.getBusHandle();

  if (m_audio.init(audio_cfg) != ESP_OK)
    return false;

  m_current_mic_gain = (float)m_record_volume; // Sync stored gain with initialized gain

  // 4. RGB LED strip — no dependencies, safe to init last
  m_leds.init((gpio_num_t)LED_STRIP_GPIO_PIN, LED_STRIP_LED_COUNT);

  m_initialized = true;
  ESP_LOGI(TAG, "Board hardware ready.");
  return true;
}

// ---------------------------------------------------------------------------
// Audio reinit — Board policy: guard against uninitialized board
// ---------------------------------------------------------------------------

esp_err_t Board::reinitAudio(uint32_t sample_rate) {
  if (!m_initialized) {
    ESP_LOGE(TAG, "Cannot reinitAudio: Board not initialized");
    return ESP_FAIL;
  }
  // Delegate the actual tear-down + reinit to AudioHal
  return m_audio.reinit(sample_rate);
}

esp_err_t Board::setHardwareSampleRate(uint32_t sample_rate) {
  if (!m_initialized) {
    ESP_LOGE(TAG, "Cannot setHardwareSampleRate: Board not initialized");
    return ESP_FAIL;
  }

  if (m_sample_rate == sample_rate) {
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Dynamic Clock Switch: Changing rate to %lu Hz...", (unsigned long)sample_rate);
  esp_err_t ret = m_audio.setHardwareSampleRate(sample_rate);
  if (ret == ESP_OK) {
    m_sample_rate = sample_rate;
  }
  return ret;
}

esp_err_t Board::setRecordGain(float db_value, bool force) {
  if (!m_initialized) {
    ESP_LOGE(TAG, "Cannot setRecordGain: Board not initialized");
    return ESP_FAIL;
  }

  if (!force && m_current_mic_gain == db_value) {
    ESP_LOGI(TAG, "setRecordGain: mic gain is already %.1f dB, skipping...", db_value);
    return ESP_OK;
  }

  esp_err_t ret = m_audio.setRecordGain(db_value);
  if (ret == ESP_OK) {
    m_current_mic_gain = db_value;
    ESP_LOGI(TAG, "setRecordGain: mic gain updated to %.1f dB", db_value);
  }
  return ret;
}
