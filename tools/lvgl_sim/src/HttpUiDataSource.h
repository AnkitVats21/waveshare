#pragma once

#include "MiniHttpClient.h"
#include "ui_view/IUiDataSource.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace lvgl_sim {

// IUiDataSource backed by polling the ESP32's HTTP API. Single background
// thread is the sole writer of m_snapshot; getSnapshot() is the reader --
// same single-writer/multi-reader shape as EmbeddedSysDb, just a std::mutex
// instead of a FreeRTOS semaphore since this runs on the host.
class HttpUiDataSource : public ui_view::IUiDataSource {
public:
    HttpUiDataSource(std::string host, uint16_t port, int poll_period_ms = 350);
    ~HttpUiDataSource() override;

    ui_view::UiSnapshot getSnapshot() const override;
    void sendPlaybackAction(const std::string& action) override;
    void setVolume(int volume_0_100) override;
    void setLed(const std::string& mode, int r, int g, int b, int speed_ms) override;

private:
    void pollLoop();
    bool fetchMusicStatus(ui_view::UiSnapshot& snap);
    bool fetchSystemDelta(ui_view::UiSnapshot& snap);
    // Slower-cadence fetches (board info, LED state, memory totals; SD
    // storage) -- these don't need every-350ms freshness like the delta,
    // so pollLoop only calls them every kSlowFetchEveryNCycles cycles.
    bool fetchSystemInit(ui_view::UiSnapshot& snap);
    bool fetchStorageInfo(ui_view::UiSnapshot& snap);
    // Downloads /music/thumbs/<track_id>.jpg (best-effort) and caches it to a
    // temp file, keyed by track id so we don't re-download every poll tick.
    void resolveAlbumArt(ui_view::UiSnapshot& snap);

    MiniHttpClient m_client;
    int m_poll_period_ms;

    std::string m_cached_art_track_id;
    std::string m_cached_art_path;

    mutable std::mutex m_mutex;
    ui_view::UiSnapshot m_snapshot;

    std::atomic<bool> m_running{true};
    std::thread m_thread;
};

} // namespace lvgl_sim
