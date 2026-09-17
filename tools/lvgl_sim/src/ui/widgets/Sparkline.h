#pragma once

#include "BarStrip.h"

#include <lvgl.h>

#include <deque>

namespace lvgl_sim::ui::widgets {

// A rolling history strip: a BarStrip whose bar heights are set from
// pushed samples (0.0-1.0) rather than an lv_anim -- the "data-driven"
// sibling of EqualizerBars, sharing the same BarStrip primitive so the
// two never duplicate the bar-row layout code. Used for the CPU
// core0/core1 sparkline; pushSample shifts the ring buffer left and
// redraws all bars.
class Sparkline {
public:
    void create(lv_obj_t* parent, int bar_count, int bar_width, int container_height,
                lv_color_t color);

    lv_obj_t* row() const { return m_strip.row(); }

    // value in [0.0, 1.0]; out-of-range values are clamped.
    void pushSample(float value);

private:
    BarStrip m_strip;
    std::deque<float> m_samples;
    int m_capacity = 0;

    void redraw();
};

} // namespace lvgl_sim::ui::widgets
