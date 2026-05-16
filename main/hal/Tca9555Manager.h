#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

/**
 * @brief Manager for the TCA9555 16-bit I2C IO Expander
 * Handles PA enable, LCD control, and LED states.
 */
class Tca9555Manager {
public:
  enum Pin {
    PA_EN = 8,    // Port 1, Pin 0
    LCD_BL = 9,   // Port 1, Pin 1
    LCD_RST = 10, // Port 1, Pin 2
    RGB_R = 1,    // Port 0, Pin 1
    RGB_G = 2,    // Port 0, Pin 2
    RGB_B = 3     // Port 0, Pin 3
  };

  static Tca9555Manager &getInstance();

  /**
   * @brief Initialize the expander on an existing I2C bus
   * @param bus_handle Handle to the initialized I2C master bus
   * @param address I2C address (usually 0x20)
   */
  esp_err_t init(i2c_master_bus_handle_t bus_handle, uint8_t address);

  /**
   * @brief Set a pin as output and set its level
   * @param pin Pin index (0-15)
   * @param level True for HIGH, False for LOW
   */
  esp_err_t setOutput(Pin pin, bool level);

  /**
   * @brief Set multiple pins as output
   * @param mask Bitmask of pins
   */
  esp_err_t setDirection(uint16_t mask, bool is_output);

private:
  Tca9555Manager() = default;
  i2c_master_dev_handle_t m_dev_handle = nullptr;
  uint16_t m_output_state = 0xFFFF;
  uint16_t m_config_state = 0xFFFF; // Default all inputs

  esp_err_t writeReg16(uint8_t reg, uint16_t value);

  // TCA9555 Registers
  static constexpr uint8_t REG_INPUT_PORT0 = 0x00;
  static constexpr uint8_t REG_OUTPUT_PORT0 = 0x02;
  static constexpr uint8_t REG_CONFIG_PORT0 = 0x06;
};
