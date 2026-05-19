#include "hal/input/ExpanderKeyInput.h"
#include "driver/gpio.h"

ExpanderKeyInput::ExpanderKeyInput(IoExpander &expander)
    : m_expander(expander) {
  // Configure KEY_2 (GPIO 0) as input with pull-up on ESP32-S3
  gpio_config_t io_conf = {};
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pin_bit_mask = (1ULL << GPIO_NUM_0);
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&io_conf);

  // Configure Key 3, 4, 5 (pins 9, 10, 11) as INPUTS on the TCA9555 IO expander
  m_expander.setDirection((1u << 9) | (1u << 10) | (1u << 11), false /* input */);
}

const ExpanderKeyInput::KeyConfig *ExpanderKeyInput::findKey(KeyId key) const {
  for (const auto &k : m_keys) {
    if (k.id == key)
      return &k;
  }
  return nullptr;
}

bool ExpanderKeyInput::isPressed(KeyId key) {
  if (key == KeyId::KEY_1) {
    // KEY_1 is CHIP_PU (hardware reset button), we cannot read it in software.
    return false;
  }
  if (key == KeyId::KEY_2) {
    // KEY_2 is GPIO0 (active-low boot button on ESP32-S3)
    return gpio_get_level(GPIO_NUM_0) == 0;
  }

  const KeyConfig *cfg = findKey(key);
  if (!cfg)
    return false;

  bool level = m_expander.readPin(cfg->pin);
  return cfg->active_low ? !level : level;
}