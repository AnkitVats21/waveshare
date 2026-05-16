#pragma once

#include "esp_err.h"
#include <cstddef>

/**
 * @brief HAL driver for SD card mounting via the SDMMC peripheral.
 *
 * Wraps esp_vfs_fat_sdmmc_mount and the associated slot/host configuration.
 * Does not contain filesystem-level logic (that belongs in services).
 */
class SdCardManager {
public:
  /**
   * @brief Mount the SD card at the given VFS mount point.
   * @param mount_point  e.g. "/sdcard"
   * @param max_files    Maximum number of simultaneously open files
   * @return ESP_OK on success
   */
  esp_err_t mount(const char *mount_point, size_t max_files);

  bool isMounted() const { return m_mounted; }

private:
  bool m_mounted = false;

  static constexpr const char *TAG = "SdCard";
};
