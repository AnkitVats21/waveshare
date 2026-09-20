#pragma once

#include "ui_view/IUiDataSource.h"
#include "ui_view/UiSnapshot.h"

#include "widgets/DetailSheet.h"
#include "widgets/GroupCarousel.h"
#include "widgets/HBar.h"
#include "widgets/LedColorSheet.h"
#include "widgets/MusicPage.h"
#include "widgets/Sparkline.h"
#include "widgets/StatRow.h"

#include <lvgl.h>

#include <functional>
#include <string>

namespace lvgl_sim::ui {

// Home screen: a topbar (Wi-Fi status icon + temperature,
// tappable for detail) above a 4-page Music/System/Ambient/Controls
// GroupCarousel + the LedColorSheet popup. There's no dedicated Network
// page -- Wi-Fi status lives in the always-visible topbar
// instead, so a value that used to need a swipe-to-check page is visible
// on every page. Music is one carousel page (title/artist, transport,
// volume) rather than a docked mini-player bar repeated on every page
// plus a separate full-screen NowPlayingScreen -- same information, one
// less navigation hop. update(snapshot) just forwards to each sub-widget
// -- no big switch statement here, per the widget-composition plan.
class DashboardScreen {
public:
    explicit DashboardScreen(ui_view::IUiDataSource& data_source);

    lv_obj_t* root() const { return m_screen; }
    void update(const ui_view::UiSnapshot& snap);

    // Navigation: dashboard has no back button of its own (it's the home
    // screen), but it originates navigation to the Assistant screen.
    void setOnOpenAssistant(std::function<void()> cb);

private:
    lv_obj_t* m_screen = nullptr;

    ui_view::IUiDataSource& m_data_source;

    widgets::GroupCarousel m_carousel;
    widgets::MusicPage m_music_page;
    widgets::LedColorSheet m_led_sheet;

    // Topbar status icons + their shared detail popup
    lv_obj_t* m_wifi_icon = nullptr;
    widgets::DetailSheet m_status_sheet;
    std::string m_wifi_detail_text;

    // System page
    widgets::StatRow m_cpu_row;
    widgets::Sparkline m_cpu0_spark;
    widgets::Sparkline m_cpu1_spark;
    widgets::HBar m_memory_bar;
    widgets::HBar m_storage_bar;

    // Ambient page
    widgets::StatRow m_weather_row;
    widgets::StatRow m_assistant_row;

    // Controls page
    widgets::StatRow m_led_row;
    lv_obj_t* m_led_swatch = nullptr;

    lv_color_t m_led_color = LV_COLOR_MAKE(0x7c, 0x4d, 0xff);
    std::string m_led_mode = "solid";

    std::function<void()> m_on_open_assistant;

    static void onAssistantRowClicked(lv_event_t* e);
    static void onLedRowClicked(lv_event_t* e);
    static void onWifiIconClicked(lv_event_t* e);
};

} // namespace lvgl_sim::ui
