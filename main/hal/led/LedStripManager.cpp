#include "hal/led/LedStripManager.h"
#include "esp_log.h"

esp_err_t LedStripManager::init(gpio_num_t gpio, uint32_t led_count) {
  m_led_count = led_count;

  led_strip_config_t strip_config = {};
  strip_config.strip_gpio_num  = gpio;
  strip_config.max_leds        = led_count;
  strip_config.led_pixel_format = LED_PIXEL_FORMAT_GRB;
  strip_config.led_model       = LED_MODEL_WS2812;
  strip_config.flags.invert_out = false;

  led_strip_rmt_config_t rmt_config = {};
  rmt_config.clk_src       = RMT_CLK_SRC_DEFAULT;
  rmt_config.resolution_hz = 10 * 1000 * 1000; // 10 MHz
  rmt_config.flags.with_dma = false;

  esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &m_led_strip);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize LED strip: %s", esp_err_to_name(ret));
    return ret;
  }

  led_strip_clear(m_led_strip);
  ESP_LOGI(TAG, "RGB LED strip initialized on GPIO %d (%lu LEDs)", (int)gpio, led_count);
  return ESP_OK;
}

void LedStripManager::setPixel(uint32_t index, uint32_t r, uint32_t g, uint32_t b) {
  if (m_led_strip) {
    led_strip_set_pixel(m_led_strip, index, r, g, b);
  }
}

void LedStripManager::setAll(uint32_t r, uint32_t g, uint32_t b) {
  if (m_led_strip) {
    for (uint32_t i = 0; i < m_led_count; i++) {
      led_strip_set_pixel(m_led_strip, i, r, g, b);
    }
    led_strip_refresh(m_led_strip);
  }
}

void LedStripManager::refresh() {
  if (m_led_strip) {
    led_strip_refresh(m_led_strip);
  }
}

void LedStripManager::clear() {
  if (m_led_strip) {
    led_strip_clear(m_led_strip);
    led_strip_refresh(m_led_strip);
  }
}
