#pragma once

// Plain-C++ view model shared between the on-device UI (future) and the
// host LVGL emulator (tools/lvgl_sim). No ESP-IDF / FreeRTOS includes here:
// this header must compile standalone on a desktop toolchain.

#include <cstdint>
#include <string>

namespace ui_view {

enum class MusicState {
    Idle,
    Streaming,
    Local,
    Paused,
};

struct UiMusicStatus {
    MusicState state = MusicState::Idle;
    std::string track_id;
    std::string title;
    std::string artist;
    int duration_sec = 0;
    int repeat_mode = 0;
    bool autoplay = true;
    bool caching = false;
    int queue_count = 0;

    // Local filesystem path to a downloaded album-art thumbnail (jpeg),
    // empty if none is available for the current track. Populated by
    // whichever IUiDataSource implementation can fetch one.
    std::string album_art_path;
};

struct UiSystemStatus {
    uint64_t uptime_sec = 0;
    int cpu0_pct = 0;
    int cpu1_pct = 0;
    uint32_t heap_internal_free = 0;
    uint32_t heap_psram_free = 0;
    // Total capacity, from /api/system/init (internal_total/psram_total) --
    // 0 until that slower-cadence endpoint has been fetched at least once.
    uint32_t heap_internal_total = 0;
    uint32_t heap_psram_total = 0;
    int wifi_rssi = 0;
    bool wifi_connected = false;
    std::string wifi_ssid;
};

// Mirrors the on-device LedMode enum (components/core_sysdb/include/common/led_types.h).
enum class LedMode {
    Off,
    Solid,
    Blink,
    Breath,
    Rainbow,
};

struct UiLedStatus {
    LedMode mode = LedMode::Off;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

// Backed by GET /api/storage/info (SD card mount).
struct UiStorageStatus {
    bool mounted = false;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
};

struct UiSnapshot {
    UiMusicStatus music;
    UiSystemStatus system;
    UiLedStatus led;
    UiStorageStatus storage;
    // True when a connection to the data source is currently healthy.
    bool data_source_online = false;

    bool isPlaying() const { return music.state != MusicState::Idle; }
};

} // namespace ui_view
