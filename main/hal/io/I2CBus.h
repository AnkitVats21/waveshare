#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

/**
 * @brief HAL driver for the I2C master bus.
 *
 * Initializes and owns the i2c_master_bus_handle_t.
 * All I2C peripherals (IoExpander, codecs) obtain the handle via getBusHandle().
 */
class I2CBus {
public:
  /**
   * @brief Initialize the I2C master bus.
   * @param port I2C port number (I2C_NUM_0 / I2C_NUM_1)
   * @param sda  GPIO for SDA
   * @param scl  GPIO for SCL
   * @return ESP_OK on success
   */
  esp_err_t init(i2c_port_t port, gpio_num_t sda, gpio_num_t scl);

  i2c_master_bus_handle_t getBusHandle() const { return m_bus; }
  bool isInitialized() const { return m_bus != nullptr; }

private:
  i2c_master_bus_handle_t m_bus = nullptr;

  static constexpr const char *TAG = "I2CBus";
};
