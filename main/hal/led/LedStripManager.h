#pragma once
#include "led_strip.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class LedStripManager {
public:
  LedStripManager();
  ~LedStripManager();

  esp_err_t init(gpio_num_t gpio, uint32_t led_count);
  void setPixel(uint32_t index, uint32_t r, uint32_t g, uint32_t b);
  void setAll(uint32_t r, uint32_t g, uint32_t b);
  void refresh();
  void clear();
  bool isInitialized() const { return m_led_strip != nullptr; }

private:
  static const char* TAG;
  uint32_t m_led_count = 0;
  led_strip_handle_t m_led_strip = nullptr;
  SemaphoreHandle_t m_hal_mutex = nullptr; // Protects hardware states
};
