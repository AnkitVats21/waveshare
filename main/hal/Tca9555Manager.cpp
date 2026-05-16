#include "Tca9555Manager.h"
#include "common/AppLogger.h"

Tca9555Manager &Tca9555Manager::getInstance() {
  static Tca9555Manager instance;
  return instance;
}

esp_err_t Tca9555Manager::init(i2c_master_bus_handle_t bus_handle,
                               uint8_t address) {
  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.device_address = address;
  dev_cfg.scl_speed_hz = 100000;

  esp_err_t ret =
      i2c_master_bus_add_device(bus_handle, &dev_cfg, &m_dev_handle);
  if (ret != ESP_OK) {
    LOGE_HAL("Failed to add TCA9555 to I2C bus");
    return ret;
  }

  LOGI_HAL("Tca9555 IO Expander initialized at 0x%02X", address);
  return ESP_OK;
}

esp_err_t Tca9555Manager::setOutput(Pin pin, bool level) {
  if (!m_dev_handle)
    return ESP_ERR_INVALID_STATE;

  // 1. Ensure pin is configured as output
  uint16_t new_config = m_config_state & ~(1 << (uint8_t)pin);
  if (new_config != m_config_state) {
    m_config_state = new_config;
    writeReg16(REG_CONFIG_PORT0, m_config_state);
  }

  // 2. Set the level
  if (level) {
    m_output_state |= (1 << (uint8_t)pin);
  } else {
    m_output_state &= ~(1 << (uint8_t)pin);
  }

  return writeReg16(REG_OUTPUT_PORT0, m_output_state);
}

esp_err_t Tca9555Manager::writeReg16(uint8_t reg, uint16_t value) {
  uint8_t data[3];
  data[0] = reg;
  data[1] = value & 0xFF;        // Port 0
  data[2] = (value >> 8) & 0xFF; // Port 1

  return i2c_master_transmit(m_dev_handle, data, 3, 100);
}
