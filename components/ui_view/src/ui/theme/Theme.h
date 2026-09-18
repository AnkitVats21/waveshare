#pragma once

// Shared color tokens + spacing/font constants for the lvgl_sim UI. Colors
// are lifted verbatim from the original NowPlayingScreen.cpp (already
// approved/proven) rather than re-invented here.

#include <lvgl.h>

namespace lvgl_sim::ui::theme {

constexpr lv_color_t kBg = LV_COLOR_MAKE(0x0b, 0x0d, 0x12);
constexpr lv_color_t kCard = LV_COLOR_MAKE(0x15, 0x19, 0x22);
constexpr lv_color_t kAccent = LV_COLOR_MAKE(0x7c, 0x4d, 0xff);
constexpr lv_color_t kAccent2 = LV_COLOR_MAKE(0x00, 0xd9, 0xc0);
constexpr lv_color_t kTextMuted = LV_COLOR_MAKE(0x9a, 0xa4, 0xaf);

// Severity colors for stat bars (memory/storage/cpu): green under 60%,
// amber 60-85%, red above that.
constexpr lv_color_t kGood = LV_COLOR_MAKE(0x2e, 0xcc, 0x71);
constexpr lv_color_t kWarn = LV_COLOR_MAKE(0xf5, 0xa6, 0x23);
constexpr lv_color_t kBad = LV_COLOR_MAKE(0xe7, 0x4c, 0x3c);

inline lv_color_t severityColor(int pct) {
    if (pct >= 85) return kBad;
    if (pct >= 60) return kWarn;
    return kGood;
}

// Spacing/font tokens. Every widget sizes itself off these (never a bare
// pixel literal) so the same widget code renders sanely at both the real
// 320x240 panel resolution and a larger desktop-dev resolution -- flip
// kScreenWidth/kScreenHeight in main.cpp and these scale the chrome, not
// the widget code.
struct Metrics {
    int spacing_xs = 4;
    int spacing_sm = 8;
    int spacing_md = 12;
    int spacing_lg = 20;

    int radius_sm = 8;
    int radius_lg = 16;

    int icon_btn_sm = 32;
    int icon_btn_md = 40;
    int icon_btn_lg = 56;

    const lv_font_t* font_sm = &lv_font_montserrat_14;
    const lv_font_t* font_md = &lv_font_montserrat_18;
    const lv_font_t* font_lg = &lv_font_montserrat_24;
    const lv_font_t* font_xl = &lv_font_montserrat_32;

    // Screen-edge padding, bar/track thickness, and the Now Playing album-art
    // diameter. These need one explicit pixel value per density (rather than
    // a plain LV_PCT) because a circle has to stay square regardless of the
    // parent's resolved flex width, and because "full screen" padding differs
    // wildly in absolute terms between a 320x240 panel and a desktop-dev
    // window -- everything else on this screen (labels, sliders, the right
    // column) is still sized with LV_PCT/flex_grow against these, not a
    // second layer of literals.
    int screen_pad = 40;
    int bar_thickness = 8;
    int now_playing_art_size = 260;

    // Scaled down for the real 320x240 panel; construct with small=true
    // in that case. Desktop dev keeps the roomier defaults above.
    static Metrics forScreen(int width, int height) {
        Metrics m;
        if (width <= 480 || height <= 400) {
            m.spacing_xs = 2;
            m.spacing_sm = 4;
            m.spacing_md = 6;
            m.spacing_lg = 10;
            m.radius_sm = 6;
            m.radius_lg = 10;
            m.icon_btn_sm = 22;
            m.icon_btn_md = 28;
            m.icon_btn_lg = 36;
            m.font_sm = &lv_font_montserrat_14;
            m.font_md = &lv_font_montserrat_14;
            m.font_lg = &lv_font_montserrat_18;
            m.font_xl = &lv_font_montserrat_24;
            m.screen_pad = 10;
            m.bar_thickness = 4;
            m.now_playing_art_size = 100;
        }
        return m;
    }
};

} // namespace lvgl_sim::ui::theme
