#include "hal/led/LedStripManager.h"
#include "esp_log.h"

const char* LedStripManager::TAG = "LedStripMgr";

LedStripManager::LedStripManager() {
    m_hal_mutex = xSemaphoreCreateMutex();
}

LedStripManager::~LedStripManager() {
    if (m_hal_mutex) {
        vSemaphoreDelete(m_hal_mutex);
    }
}

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
  
  // CRITICAL FIX FOR GEMINI VOICE PIECE:
  // Enables hardware Direct Memory Access. The CPU sets up the buffer, 
  // fires the RMT peripheral, and instantly yields back to your audio/network tasks.
  rmt_config.flags.with_dma = true; 

  xSemaphoreTake(m_hal_mutex, portMAX_DELAY);
  esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &m_led_strip);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize LED strip: %s", esp_err_to_name(ret));
    xSemaphoreGive(m_hal_mutex);
    return ret;
  }

  led_strip_clear(m_led_strip);
  led_strip_refresh(m_led_strip);
  xSemaphoreGive(m_hal_mutex);

  ESP_LOGI(TAG, "RGB LED strip initialized via DMA on GPIO %d (%lu LEDs)", (int)gpio, led_count);
  return ESP_OK;
}

void LedStripManager::setPixel(uint32_t index, uint32_t r, uint32_t g, uint32_t b) {
  if (m_led_strip && xSemaphoreTake(m_hal_mutex, portMAX_DELAY) == pdTRUE) {
    led_strip_set_pixel(m_led_strip, index, r, g, b);
    xSemaphoreGive(m_hal_mutex);
  }
}

void LedStripManager::setAll(uint32_t r, uint32_t g, uint32_t b) {
  if (m_led_strip && xSemaphoreTake(m_hal_mutex, portMAX_DELAY) == pdTRUE) {
    for (uint32_t i = 0; i < m_led_count; i++) {
      led_strip_set_pixel(m_led_strip, i, r, g, b);
    }
    xSemaphoreGive(m_hal_mutex);
  }
}

void LedStripManager::refresh() {
  if (m_led_strip && xSemaphoreTake(m_hal_mutex, portMAX_DELAY) == pdTRUE) {
    led_strip_refresh(m_led_strip);
    xSemaphoreGive(m_hal_mutex);
  }
}

void LedStripManager::clear() {
  if (m_led_strip && xSemaphoreTake(m_hal_mutex, portMAX_DELAY) == pdTRUE) {
    led_strip_clear(m_led_strip);
    led_strip_refresh(m_led_strip);
    xSemaphoreGive(m_hal_mutex);
  }
}
