#include "hal/io/IoExpander.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "esp_log.h"

esp_err_t IoExpander::init(i2c_master_bus_handle_t bus_handle,
                           uint8_t address) {
  // Create the high-level expander handle (used for set_dir / set_level)
  esp_err_t ret =
      esp_io_expander_new_i2c_tca95xx_16bit(bus_handle, address, &m_expander);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create TCA9555 expander: %s",
             esp_err_to_name(ret));
    return ret;
  }

  // Also create a low-level I2C device handle for direct register writes
  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.device_address = address;
  dev_cfg.scl_speed_hz = 100000;

  ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &m_dev_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add TCA9555 to I2C bus: %s", esp_err_to_name(ret));
    return ret;
  }

  ESP_LOGI(TAG, "IoExpander (TCA9555) initialized at 0x%02X", address);
  return ESP_OK;
}

esp_err_t IoExpander::setOutput(Pin pin, bool level) {
  if (!m_dev_handle)
    return ESP_ERR_INVALID_STATE;

  // Ensure the pin is configured as output
  uint16_t new_config = m_config_state & ~(1u << (uint8_t)pin);
  if (new_config != m_config_state) {
    m_config_state = new_config;
    writeReg16(REG_CONFIG_PORT0, m_config_state);
  }

  // Write the output level
  if (level) {
    m_output_state |= (1u << (uint8_t)pin);
  } else {
    m_output_state &= ~(1u << (uint8_t)pin);
  }

  return writeReg16(REG_OUTPUT_PORT0, m_output_state);
}

esp_err_t IoExpander::setDirection(uint16_t mask, bool is_output) {
  if (!m_dev_handle)
    return ESP_ERR_INVALID_STATE;

  if (is_output) {
    m_config_state &= ~mask;
  } else {
    m_config_state |= mask;
  }
  return writeReg16(REG_CONFIG_PORT0, m_config_state);
}

esp_err_t IoExpander::setOutputMask(uint16_t mask, bool level) {
  if (!m_dev_handle)
    return ESP_ERR_INVALID_STATE;

  if (level) {
    m_output_state |= mask;
  } else {
    m_output_state &= ~mask;
  }
  return writeReg16(REG_OUTPUT_PORT0, m_output_state);
}

esp_err_t IoExpander::writeReg16(uint8_t reg, uint16_t value) {
  uint8_t data[3];
  data[0] = reg;
  data[1] = value & 0xFF;        // Port 0 (lower byte)
  data[2] = (value >> 8) & 0xFF; // Port 1 (upper byte)

  return i2c_master_transmit(m_dev_handle, data, 3, 100);
}

esp_err_t IoExpander::readInputs(uint16_t &value) {
  if (!m_dev_handle)
    return ESP_ERR_INVALID_STATE;

  uint8_t reg = REG_INPUT_PORT0;
  uint8_t data[2] = {0};

  esp_err_t ret =
      i2c_master_transmit_receive(m_dev_handle, &reg, 1, data, 2, 100);

  if (ret != ESP_OK)
    return ret;

  value = data[0] | (data[1] << 8);
  return ESP_OK;
}

bool IoExpander::readPin(uint8_t pin) {
  uint16_t value = 0;

  if (readInputs(value) != ESP_OK)
    return false;

  return (value & (1u << pin)) != 0;
}