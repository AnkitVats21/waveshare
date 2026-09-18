#include "ui_view/SysDbUiDataSource.h"
#include "core_sysdb/EmbeddedSysDb.h"
#include "media_player/MusicPlaybackService.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

namespace ui_view {

static const char* TAG = "SysDbUiDataSource";

SysDbUiDataSource::SysDbUiDataSource() {
    ESP_LOGI(TAG, "SysDbUiDataSource initialized (direct in-memory EmbeddedSysDb binding)");
}

UiSnapshot SysDbUiDataSource::getSnapshot() const {
    UiSnapshot snap_ui;
    const SystemState state = EmbeddedSysDb::getInstance().snapshot();

    // ── Music Status ─────────────────────────────────────────────────────────
    switch (state.media.state) {
        case MediaPlaybackState::PLAYING:
        case MediaPlaybackState::BUFFERING:
        case MediaPlaybackState::RESOLVING:
            snap_ui.music.state = MusicState::Streaming;
            break;
        case MediaPlaybackState::PAUSED:
            snap_ui.music.state = MusicState::Paused;
            break;
        case MediaPlaybackState::IDLE:
        case MediaPlaybackState::ERROR_STATE:
        default:
            snap_ui.music.state = MusicState::Idle;
            break;
    }

    snap_ui.music.track_id     = state.media.active_song_id;
    snap_ui.music.title        = state.media.title;
    snap_ui.music.artist       = state.media.artist;
    snap_ui.music.duration_sec = state.media.duration_ms / 1000;
    snap_ui.music.repeat_mode  = state.media.repeat_mode;
    snap_ui.music.autoplay     = state.media.autoplay_enabled;
    snap_ui.music.caching      = state.media.cache_downloads;

    // ── System Status ────────────────────────────────────────────────────────
    snap_ui.system.uptime_sec         = esp_timer_get_time() / 1000000ULL;
    snap_ui.system.wifi_connected     = state.system.wifi_connected;
    snap_ui.system.heap_internal_free = esp_get_free_internal_heap_size();
    snap_ui.system.heap_psram_free    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snap_ui.system.heap_internal_total = 512 * 1024;
    snap_ui.system.heap_psram_total    = 8 * 1024 * 1024;

    // ── LED Status ───────────────────────────────────────────────────────────
    switch (state.led.mode) {
        case ::LedMode::SOLID:   snap_ui.led.mode = LedMode::Solid;   break;
        case ::LedMode::BLINK:   snap_ui.led.mode = LedMode::Blink;   break;
        case ::LedMode::BREATH:  snap_ui.led.mode = LedMode::Breath;  break;
        case ::LedMode::RAINBOW: snap_ui.led.mode = LedMode::Rainbow; break;
        case ::LedMode::OFF:
        default:                 snap_ui.led.mode = LedMode::Off;     break;
    }
    snap_ui.led.r = state.led.color.r;
    snap_ui.led.g = state.led.color.g;
    snap_ui.led.b = state.led.color.b;

    // Online status
    snap_ui.data_source_online = true;

    return snap_ui;
}

void SysDbUiDataSource::sendPlaybackAction(const std::string& action) {
    ESP_LOGI(TAG, "sendPlaybackAction: %s", action.c_str());
    auto& music = MusicPlaybackService::getInstance();

    if (action == "play" || action == "resume") {
        music.resume();
    } else if (action == "pause") {
        music.pause();
    } else if (action == "toggle") {
        const SystemState state = EmbeddedSysDb::getInstance().snapshot();
        if (state.media.state == MediaPlaybackState::PLAYING) {
            music.pause();
        } else {
            music.resume();
        }
    } else if (action == "next") {
        music.next();
    } else if (action == "previous") {
        music.previous();
    } else if (action == "stop") {
        music.stop();
    }
}

void SysDbUiDataSource::setVolume(int volume_0_100) {
    if (volume_0_100 < 0) volume_0_100 = 0;
    if (volume_0_100 > 100) volume_0_100 = 100;
    ESP_LOGI(TAG, "setVolume: %d", volume_0_100);

    EmbeddedSysDb::getInstance().mutate([volume_0_100](SystemState& s) {
        s.audio.speaker_volume = volume_0_100;
    });
}

void SysDbUiDataSource::setLed(const std::string& mode, int r, int g, int b, int speed_ms) {
    ESP_LOGI(TAG, "setLed: mode=%s, rgb=(%d,%d,%d), speed=%d", mode.c_str(), r, g, b, speed_ms);

    ::LedMode target_mode = ::LedMode::OFF;
    if (mode == "solid") target_mode = ::LedMode::SOLID;
    else if (mode == "blink") target_mode = ::LedMode::BLINK;
    else if (mode == "breath") target_mode = ::LedMode::BREATH;
    else if (mode == "rainbow") target_mode = ::LedMode::RAINBOW;

    EmbeddedSysDb::getInstance().mutate([target_mode, r, g, b, speed_ms](SystemState& s) {
        s.led.mode = target_mode;
        s.led.color = RgbColor{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
        s.led.speed_ms = speed_ms > 0 ? speed_ms : 500;
    });
}

} // namespace ui_view
