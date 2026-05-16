#include "hal/storage/SdCardManager.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "hal/Board_defs.h"
#include "sdmmc_cmd.h"

esp_err_t SdCardManager::mount(const char *mount_point, size_t max_files) {
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
  mount_config.format_if_mount_failed  = false;
  mount_config.max_files               = (int)max_files;
  mount_config.allocation_unit_size    = 16 * 1024;
  mount_config.disk_status_check_enable = false;

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = SDMMC_BUS_WIDTH;
  slot_config.clk   = (gpio_num_t)GPIO_SDMMC_CLK;
  slot_config.cmd   = (gpio_num_t)GPIO_SDMMC_CMD;
  slot_config.d0    = (gpio_num_t)GPIO_SDMMC_D0;
  slot_config.d1    = (gpio_num_t)GPIO_SDMMC_D1;
  slot_config.d2    = (gpio_num_t)GPIO_SDMMC_D2;
  slot_config.d3    = (gpio_num_t)GPIO_SDMMC_D3;
  slot_config.cd    = (gpio_num_t)GPIO_SDMMC_DET;
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  sdmmc_card_t *card;
  esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config,
                                           &mount_config, &card);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
    return ret;
  }

  sdmmc_card_print_info(stdout, card);
  m_mounted = true;
  return ESP_OK;
}
