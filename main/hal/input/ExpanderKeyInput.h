#pragma once

#include "hal/io/IoExpander.h"
#include <cstdint>

class ExpanderKeyInput {
public:
  enum class KeyId : uint8_t { KEY_1, KEY_2, KEY_3, KEY_4, KEY_5 };

  struct KeyConfig {
    KeyId id;
    uint8_t pin;
    bool active_low;
  };

  explicit ExpanderKeyInput(IoExpander &expander);

  bool isPressed(KeyId key);

private:
  IoExpander &m_expander;

  static constexpr KeyConfig m_keys[] = {
      {KeyId::KEY_3, 9, true},
      {KeyId::KEY_4, 10, true},
      {KeyId::KEY_5, 11, true},
  };

  const KeyConfig *findKey(KeyId key) const;
};