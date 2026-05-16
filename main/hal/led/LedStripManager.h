#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include "led_strip.h"
#include <cstdint>

/**
 * @brief HAL driver for the WS2812 RGB LED strip via RMT peripheral.
 *
 * Owns the led_strip_handle_t and all RMT configuration.
 * Does not contain any application-level color policies.
 */
class LedStripManager {
public:
  /**
   * @brief Initialize the LED strip on the given GPIO.
   * @param gpio     GPIO pin connected to the strip's data line
   * @param led_count Number of LEDs in the strip
   * @return ESP_OK on success
   */
  esp_err_t init(gpio_num_t gpio, uint32_t led_count);

  /**
   * @brief Set a single pixel color. Does NOT call refresh().
   */
  void setPixel(uint32_t index, uint32_t r, uint32_t g, uint32_t b);

  /**
   * @brief Set all pixels to the same color and immediately refresh.
   */
  void setAll(uint32_t r, uint32_t g, uint32_t b);

  /**
   * @brief Push pending pixel changes to hardware.
   */
  void refresh();

  /**
   * @brief Clear all pixels (set to black) and refresh.
   */
  void clear();

  bool isInitialized() const { return m_led_strip != nullptr; }

private:
  led_strip_handle_t m_led_strip = nullptr;
  uint32_t m_led_count = 0;

  static constexpr const char *TAG = "LedStrip";
};
