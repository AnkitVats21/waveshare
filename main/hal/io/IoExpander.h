#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_io_expander.h"
#include <cstdint>

/**
 * @brief HAL driver for the TCA9555 16-bit I2C IO Expander.
 *
 * Replaces the standalone Tca9555Manager — same low-level implementation,
 * now owned by Board as a composed member. Added board-level convenience
 * wrappers (setPowerRail, setLcdBacklight) used by Board::begin().
 */
class IoExpander {
public:
  enum Pin : uint8_t {
    PA_EN   = 8,  // Port 1, Pin 0 — Power amplifier enable
    LCD_BL  = 9,  // Port 1, Pin 1 — LCD backlight
    LCD_RST = 10, // Port 1, Pin 2 — LCD reset
    RGB_R   = 1,  // Port 0, Pin 1
    RGB_G   = 2,  // Port 0, Pin 2
    RGB_B   = 3   // Port 0, Pin 3
  };

  /**
   * @brief Initialize the expander on an existing I2C bus.
   * @param bus_handle Handle to an initialized I2C master bus
   * @param address    7-bit I2C address of the TCA9555
   */
  esp_err_t init(i2c_master_bus_handle_t bus_handle, uint8_t address);

  /**
   * @brief Set an individual pin direction to output and write its level.
   * @param pin   Pin enum value
   * @param level true = HIGH, false = LOW
   */
  esp_err_t setOutput(Pin pin, bool level);

  /**
   * @brief Set the direction of a bitmask of pins.
   * @param mask       16-bit pin mask
   * @param is_output  true = output, false = input
   */
  esp_err_t setDirection(uint16_t mask, bool is_output);

  // -- Board-level convenience wrappers --

  /** @brief Enable or disable the on-board power amplifier rail. */
  esp_err_t setPowerRail(bool enable) { return setOutput(PA_EN, enable); }

  /** @brief Control the LCD backlight. */
  esp_err_t setLcdBacklight(bool on) { return setOutput(LCD_BL, on); }

  /** @brief Reset the LCD panel (active-high reset pulse). */
  esp_err_t setLcdReset(bool asserted) { return setOutput(LCD_RST, asserted); }

  bool isInitialized() const { return m_dev_handle != nullptr; }

  /** @brief Return the raw expander handle for any code that still needs it. */
  esp_io_expander_handle_t getRawHandle() const { return m_expander; }

private:
  i2c_master_dev_handle_t m_dev_handle = nullptr;
  esp_io_expander_handle_t m_expander  = nullptr;

  uint16_t m_output_state = 0xFFFF;
  uint16_t m_config_state = 0xFFFF; // default all inputs

  esp_err_t writeReg16(uint8_t reg, uint16_t value);

  // TCA9555 register map
  static constexpr uint8_t REG_INPUT_PORT0  = 0x00;
  static constexpr uint8_t REG_OUTPUT_PORT0 = 0x02;
  static constexpr uint8_t REG_CONFIG_PORT0 = 0x06;

  static constexpr const char *TAG = "IoExpander";
};
