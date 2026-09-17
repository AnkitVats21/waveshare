#include "HttpUiDataSource.h"

#include <ArduinoJson.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

using ui_view::MusicState;
using ui_view::UiSnapshot;

namespace lvgl_sim {

namespace {

MusicState parseMusicState(const char* s) {
    if (!s) return MusicState::Idle;
    if (std::strcmp(s, "STREAMING") == 0) return MusicState::Streaming;
    if (std::strcmp(s, "LOCAL") == 0) return MusicState::Local;
    if (std::strcmp(s, "PAUSED") == 0) return MusicState::Paused;
    return MusicState::Idle;
}

} // namespace

HttpUiDataSource::HttpUiDataSource(std::string host, uint16_t port, int poll_period_ms)
    : m_client(std::move(host), port), m_poll_period_ms(poll_period_ms) {
    m_thread = std::thread(&HttpUiDataSource::pollLoop, this);
}

HttpUiDataSource::~HttpUiDataSource() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

UiSnapshot HttpUiDataSource::getSnapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

void HttpUiDataSource::sendPlaybackAction(const std::string& action) {
    // Called directly from the LVGL button click handler -- must not block
    // the UI thread on network I/O, so the actual POST runs on its own
    // short-lived thread. m_client has no shared mutable state across
    // requests, so calling it concurrently with the poll thread is safe.
    std::thread([this, action] {
        std::string body = "{\"action\":\"" + action + "\"}";
        std::string resp;
        m_client.post("/api/music/control", body, resp);
    }).detach();
}

void HttpUiDataSource::setVolume(int volume_0_100) {
    std::thread([this, volume_0_100] {
        std::string body = "{\"action\":\"volume\",\"value\":" + std::to_string(volume_0_100) + "}";
        std::string resp;
        m_client.post("/api/audio/volume", body, resp);
    }).detach();
}

void HttpUiDataSource::setLed(const std::string& mode, int r, int g, int b, int speed_ms) {
    std::thread([this, mode, r, g, b, speed_ms] {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "{\"mode\":\"%s\",\"r\":%d,\"g\":%d,\"b\":%d,\"speed_ms\":%d}",
                      mode.c_str(), r, g, b, speed_ms);
        std::string resp;
        m_client.post("/api/led/set", buf, resp);
    }).detach();
}

bool HttpUiDataSource::fetchMusicStatus(UiSnapshot& snap) {
    std::string resp;
    if (!m_client.get("/api/music/status", resp)) return false;

    JsonDocument doc;
    if (deserializeJson(doc, resp) != DeserializationError::Ok) return false;

    snap.music.state = parseMusicState(doc["state"] | "IDLE");
    JsonObjectConst track = doc["current_track"];
    snap.music.track_id = std::string(track["id"] | "");
    snap.music.title = std::string(track["title"] | "");
    snap.music.artist = std::string(track["artist"] | "");
    snap.music.duration_sec = track["duration"] | 0;
    snap.music.repeat_mode = doc["repeat_mode"] | 0;
    snap.music.autoplay = doc["autoplay"] | true;
    snap.music.caching = doc["caching"] | false;
    snap.music.queue_count = doc["queue_count"] | 0;
    return true;
}

bool HttpUiDataSource::fetchSystemDelta(UiSnapshot& snap) {
    std::string resp;
    if (!m_client.get("/api/system/delta", resp)) return false;

    JsonDocument doc;
    if (deserializeJson(doc, resp) != DeserializationError::Ok) return false;

    snap.system.uptime_sec = doc["up"] | 0;
    snap.system.cpu0_pct = doc["c0"] | 0;
    snap.system.cpu1_pct = doc["c1"] | 0;
    snap.system.heap_internal_free = doc["sram"] | 0;
    snap.system.heap_psram_free = doc["psram"] | 0;
    snap.system.wifi_rssi = doc["rssi"] | 0;
    snap.system.wifi_connected = snap.system.wifi_rssi != 0;
    return true;
}

namespace {
ui_view::LedMode parseLedMode(int raw) {
    switch (raw) {
        case 1: return ui_view::LedMode::Solid;
        case 2: return ui_view::LedMode::Blink;
        case 3: return ui_view::LedMode::Breath;
        case 4: return ui_view::LedMode::Rainbow;
        default: return ui_view::LedMode::Off;
    }
}
} // namespace

bool HttpUiDataSource::fetchSystemInit(UiSnapshot& snap) {
    std::string resp;
    if (!m_client.get("/api/system/init", resp)) return false;

    JsonDocument doc;
    if (deserializeJson(doc, resp) != DeserializationError::Ok) return false;

    snap.system.heap_internal_total = doc["internal_total"] | 0;
    snap.system.heap_psram_total = doc["psram_total"] | 0;

    JsonObjectConst state = doc["state"];
    snap.led.mode = parseLedMode(state["led_mode"] | 0);
    // Verified against the real device: POST /api/led/set {r,g,b} comes
    // back with led_r/led_g swapped in this same state object (the strip
    // is wired GRB, and the swap for that happens on the write path into
    // SystemState.led.color) -- un-swap here so a round-tripped color
    // matches what was actually requested/perceived, not the wire order.
    snap.led.r = static_cast<uint8_t>(static_cast<int>(state["led_g"] | 0));
    snap.led.g = static_cast<uint8_t>(static_cast<int>(state["led_r"] | 0));
    snap.led.b = static_cast<uint8_t>(static_cast<int>(state["led_b"] | 0));
    return true;
}

bool HttpUiDataSource::fetchStorageInfo(UiSnapshot& snap) {
    std::string resp;
    if (!m_client.get("/api/storage/info", resp)) return false;

    JsonDocument doc;
    if (deserializeJson(doc, resp) != DeserializationError::Ok) return false;

    snap.storage.mounted = doc["mounted"] | false;
    snap.storage.total_bytes = doc["total_bytes"] | static_cast<uint64_t>(0);
    snap.storage.free_bytes = doc["free_bytes"] | static_cast<uint64_t>(0);
    return true;
}

void HttpUiDataSource::resolveAlbumArt(UiSnapshot& snap) {
    const std::string& id = snap.music.track_id;
    if (id.empty()) {
        snap.music.album_art_path.clear();
        return;
    }

    if (id != m_cached_art_track_id) {
        m_cached_art_track_id = id;
        m_cached_art_path.clear();

        std::string jpeg;
        const std::string path = "/api/files/download?path=/music/thumbs/" + id + ".jpg";
        if (m_client.get(path, jpeg) && !jpeg.empty()) {
            const std::string file_path = "/tmp/lvgl_sim_thumb_" + id + ".jpg";
            std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
            if (out) {
                out.write(jpeg.data(), static_cast<std::streamsize>(jpeg.size()));
                if (out.good()) m_cached_art_path = file_path;
            }
        }
    }

    snap.music.album_art_path = m_cached_art_path;
}

void HttpUiDataSource::pollLoop() {
    // LED state, memory totals, and SD storage usage don't need every-tick
    // freshness like music/CPU do, and /api/system/init is a heavier
    // payload than /api/system/delta -- fetch them every Nth cycle instead
    // of every poll.
    constexpr int kSlowFetchEveryNCycles = 10;
    int cycle = 0;
    while (m_running.load()) {
        UiSnapshot next;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            next = m_snapshot;
        }

        const bool ok_music = fetchMusicStatus(next);
        const bool ok_system = fetchSystemDelta(next);
        bool online = ok_music || ok_system;
        if (ok_music) resolveAlbumArt(next);

        if (cycle % kSlowFetchEveryNCycles == 0) {
            online = fetchSystemInit(next) || online;
            online = fetchStorageInfo(next) || online;
        }
        next.data_source_online = online;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_snapshot = next;
        }

        ++cycle;
        std::this_thread::sleep_for(std::chrono::milliseconds(m_poll_period_ms));
    }
}

} // namespace lvgl_sim
