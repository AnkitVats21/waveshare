#pragma once

#include "esp_err.h"

/**
 * @brief Base class for all Hardware Abstraction Layer (HAL) modules
 */
class HalBase {
public:
  virtual ~HalBase() = default;

  /**
   * @brief Initialize the hardware module
   * @return true if successful
   */
  virtual bool begin() = 0;

  /**
   * @brief Check if the module is initialized
   * @return true if initialized
   */
  virtual bool isInitialized() const { return m_initialized; }

protected:
  bool m_initialized = false;
};
