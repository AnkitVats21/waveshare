#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"

#include "sd_protocol_types.h"

namespace sd_storage {

// Board wiring and mount options. The board layer fills this in, so the
// component knows nothing about pins.
struct SdCardConfig {
    const char* mount_point = "/sdcard";
    int max_files = 8;
    int bus_width = 1;
    int clk = -1;
    int cmd = -1;
    int d0 = -1;
    int d1 = -1;
    int d2 = -1;
    int d3 = -1;
    int cd = -1;
    size_t allocation_unit = 16 * 1024;
};

// Mounts FATFS over SDMMC. Owns the optional persistent DMA bounce buffer the
// driver uses for PSRAM transfers (CONFIG_SD_STORAGE_DMA_BUFFER_SIZE).
class SdCard {
public:
    static SdCard& instance();

    esp_err_t mount(const SdCardConfig& cfg);

    bool isMounted() const { return m_card != nullptr; }
    const char* mountPoint() const { return m_mount_point; }
    bool info(uint64_t& total_bytes, uint64_t& free_bytes) const;

private:
    SdCard() = default;
    SdCard(const SdCard&) = delete;
    SdCard& operator=(const SdCard&) = delete;

    sdmmc_card_t* m_card = nullptr;
    void* m_dma_buffer = nullptr;
    const char* m_mount_point = "/sdcard";
};

} // namespace sd_storage
