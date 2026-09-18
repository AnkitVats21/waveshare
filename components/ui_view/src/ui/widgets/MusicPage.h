#pragma once

#include "ui_view/IUiDataSource.h"
#include "ui_view/UiSnapshot.h"

#include <lvgl.h>

namespace lvgl_sim::ui::widgets {

// Music now lives as one GroupCarousel page (title/artist, transport
// controls, volume) instead of a docked mini-player bar shown on every
// page plus a separate full-screen NowPlayingScreen -- same information,
// no duplicated chrome, no extra navigation hop.
//
// Deliberately no status chip or equalizer animation: on the real 320x240
// panel every row of vertical space is scarce, and the transport buttons
// (the thing you actually need to tap) are worth more than either
// decoration -- they were pushing prev/play/next off the bottom of the
// page. Title/artist plus the STREAMING/PAUSED state (now folded into the
// play/pause icon and time label) already carries that same information.
//
// The seek bar is a mock: UiSnapshot carries duration_sec but no elapsed/
// position field yet, so -10s/+10s only nudge a locally-held percentage
// for visual feedback. Wiring them to a real seek once the backend
// supports it means adding sendSeek()-style calls here, not touching the
// layout.
class MusicPage {
public:
    void create(lv_obj_t* parent, int width, ui_view::IUiDataSource& data_source);

    void update(const ui_view::UiSnapshot& snap);

private:
    ui_view::IUiDataSource* m_data_source = nullptr;

    lv_obj_t* m_title_label = nullptr;
    lv_obj_t* m_artist_label = nullptr;

    lv_obj_t* m_progress_bar = nullptr;
    lv_obj_t* m_time_label = nullptr;
    int m_mock_seek_pct = 0;
    int m_duration_sec = 0;

    lv_obj_t* m_play_pause_btn = nullptr;
    lv_obj_t* m_play_pause_label = nullptr;

    lv_obj_t* m_volume_icon = nullptr;
    lv_obj_t* m_volume_slider = nullptr;

    void refreshTimeLabel();

    static void onSeekBackClicked(lv_event_t* e);
    static void onSeekForwardClicked(lv_event_t* e);
    static void onPrevClicked(lv_event_t* e);
    static void onPlayPauseClicked(lv_event_t* e);
    static void onNextClicked(lv_event_t* e);
    static void onVolumeChanged(lv_event_t* e);
};

} // namespace lvgl_sim::ui::widgets
