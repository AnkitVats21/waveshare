#pragma once

#include "esp_http_server.h"
#include "esp_partition.h"
#include <cstdint>

namespace Http {

/**
 * @brief A web frontend stored in flash as a single "bundle" image and served
 * straight from the memory-mapped partition (no filesystem, no copies).
 *
 * Two slots (partitions www_0 / www_1, A/B): an upload always goes to the
 * inactive slot and becomes active only after its SHA-256 verifies, so a
 * failed upload never breaks the running frontend. The active slot is kept in
 * NVS (namespace "www", key "slot").
 *
 * Bundle layout (little-endian, built by tools/webbundle/mkbundle.py):
 *   Header (80 B): magic "NXWB", u16 format=1, u16 file_count, u32 total_size,
 *                  u32 build_time, char version[32], u8 sha256[32]
 *   Entries (128 B each): char path[104], u32 offset, u32 size,
 *                         u32 flags (bit0 = gzip), u8 etag[8], u32 reserved
 *   File data (each file 4-byte aligned).
 * sha256 covers the whole bundle with the sha256 field zeroed.
 *
 * Threading: serve() runs on the httpd task; activate() must run on a task
 * with an internal-RAM stack (it writes NVS) while httpd is blocked in the
 * upload handler, so the two never overlap.
 */
class WebBundle {
public:
    static constexpr int SLOT_COUNT = 2;
    static constexpr int NO_SLOT = -1;

    struct SlotInfo {
        bool valid = false;
        char version[33] = {};
        uint32_t build_time = 0;
        uint32_t size = 0;
        uint16_t files = 0;
        char sha256[65] = {};
    };

    static WebBundle& instance();

    // Finds the slot partitions and maps the active (or else any valid) bundle.
    void init();

    bool available() const { return m_active != NO_SLOT; }
    int activeSlot() const { return m_active; }
    int uploadSlot() const;  // where the next upload goes
    const esp_partition_t* partition(int slot) const;
    SlotInfo info(int slot) const;

    // Checks the slot's header and SHA-256; on success maps it and records it
    // as active in NVS.
    esp_err_t activate(int slot);

    // Serves req->uri from the active bundle. Unknown extension-less paths get
    // /index.html (client-side routing). ESP_ERR_NOT_FOUND if there is no
    // bundle or no such file; nothing has been sent in that case.
    esp_err_t serve(httpd_req_t* req);

private:
    WebBundle() = default;
    bool mapSlot(int slot);
    void unmap();

    const esp_partition_t* m_parts[SLOT_COUNT] = {};
    int m_active = NO_SLOT;
    const uint8_t* m_base = nullptr;
    esp_partition_mmap_handle_t m_mmap = 0;
};

} // namespace Http
