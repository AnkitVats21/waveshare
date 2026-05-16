#include "hal/io/I2CBus.h"
#include "driver/gpio.h"
#include "esp_log.h"

esp_err_t I2CBus::init(i2c_port_t port, gpio_num_t sda, gpio_num_t scl) {
  if (m_bus != nullptr)
    return ESP_OK;

  i2c_master_bus_config_t bus_config = {};
  bus_config.i2c_port               = port;
  bus_config.sda_io_num             = sda;
  bus_config.scl_io_num             = scl;
  bus_config.clk_source             = I2C_CLK_SRC_DEFAULT;
  bus_config.glitch_ignore_cnt      = 7;
  bus_config.intr_priority          = 0;
  bus_config.trans_queue_depth      = 0;
  bus_config.flags.enable_internal_pullup = true;

  gpio_set_pull_mode(sda, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(scl, GPIO_PULLUP_ONLY);

  esp_err_t ret = i2c_new_master_bus(&bus_config, &m_bus);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(ret));
    return ret;
  }

  ESP_LOGI(TAG, "I2C master bus initialized (port=%d, SDA=%d, SCL=%d)",
           (int)port, (int)sda, (int)scl);
  return ESP_OK;
}
