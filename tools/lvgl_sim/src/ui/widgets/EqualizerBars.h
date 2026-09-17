#pragma once

#include "BarStrip.h"

#include <lvgl.h>

#include <vector>

namespace lvgl_sim::ui::widgets {

// Fake audio-reactive equalizer: a BarStrip whose bars are individually
// driven by infinite-repeat lv_anim_t bounces between a low and high
// height. Extracted verbatim (same animation parameters) from the
// original NowPlayingScreen inline implementation. Reused unchanged by
// AssistantScreen as its listening/speaking/thinking animation.
class EqualizerBars {
public:
    void create(lv_obj_t* parent, int bar_count, int bar_width, int container_height,
                lv_color_t color_a, lv_color_t color_b);

    lv_obj_t* row() const { return m_strip.row(); }

    void start();
    void stop();
    bool running() const { return m_running; }

private:
    BarStrip m_strip;
    std::vector<lv_anim_t> m_anims;
    bool m_running = false;

    static void animCb(void* var, int32_t value);
};

} // namespace lvgl_sim::ui::widgets
