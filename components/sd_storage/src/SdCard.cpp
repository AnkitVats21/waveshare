#include "sd_storage/SdCard.h"

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"

namespace sd_storage {

namespace {
const char* TAG = "SdCard";
}

SdCard& SdCard::instance() {
    static SdCard card;
    return card;
}

esp_err_t SdCard::mount(const SdCardConfig& cfg) {
    if (m_card) return ESP_OK;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed   = false;
    mount_config.max_files                = cfg.max_files;
    mount_config.allocation_unit_size     = cfg.allocation_unit;
    mount_config.disk_status_check_enable = false;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    // Optional permanent bounce buffer for PSRAM transfers instead of a malloc
    // and free on every read and write. Allocated at boot while internal RAM
    // is still unfragmented.
    if (CONFIG_SD_STORAGE_DMA_BUFFER_SIZE > 0 && !m_dma_buffer) {
        m_dma_buffer = heap_caps_malloc(CONFIG_SD_STORAGE_DMA_BUFFER_SIZE,
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!m_dma_buffer) {
            ESP_LOGW(TAG, "no internal RAM for the %d-byte DMA buffer; driver will allocate per transfer",
                     CONFIG_SD_STORAGE_DMA_BUFFER_SIZE);
        }
    }
    host.dma_aligned_buffer = m_dma_buffer;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = cfg.bus_width;
    slot_config.clk   = static_cast<gpio_num_t>(cfg.clk);
    slot_config.cmd   = static_cast<gpio_num_t>(cfg.cmd);
    slot_config.d0    = static_cast<gpio_num_t>(cfg.d0);
    slot_config.d1    = static_cast<gpio_num_t>(cfg.d1);
    slot_config.d2    = static_cast<gpio_num_t>(cfg.d2);
    slot_config.d3    = static_cast<gpio_num_t>(cfg.d3);
    slot_config.cd    = static_cast<gpio_num_t>(cfg.cd);
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t* card = nullptr;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(cfg.mount_point, &host, &slot_config,
                                            &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        heap_caps_free(m_dma_buffer);
        m_dma_buffer = nullptr;
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    m_mount_point = cfg.mount_point;
    m_card = card;
    return ESP_OK;
}

bool SdCard::info(uint64_t& total_bytes, uint64_t& free_bytes) const {
    total_bytes = 0;
    free_bytes = 0;
    if (!m_card) return false;
    esp_err_t err = esp_vfs_fat_info(m_mount_point, &total_bytes, &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_vfs_fat_info failed for %s: %s", m_mount_point, esp_err_to_name(err));
        return false;
    }
    return true;
}

} // namespace sd_storage
