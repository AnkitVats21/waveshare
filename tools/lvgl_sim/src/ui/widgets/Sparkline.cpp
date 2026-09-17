#include "Sparkline.h"

#include <algorithm>

namespace lvgl_sim::ui::widgets {

void Sparkline::create(lv_obj_t* parent, int bar_count, int bar_width, int container_height,
                        lv_color_t color) {
    m_capacity = bar_count;
    // Same color for both "slots" -- Sparkline doesn't alternate colors
    // like EqualizerBars, it's a single series per instance.
    m_strip.create(parent, bar_count, bar_width, container_height, color, color);
    m_strip.setHidden(false);
}

void Sparkline::pushSample(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    m_samples.push_back(value);
    while (static_cast<int>(m_samples.size()) > m_capacity) m_samples.pop_front();
    redraw();
}

void Sparkline::redraw() {
    const int n = m_strip.count();
    const int have = static_cast<int>(m_samples.size());
    // Right-align: newest sample in the rightmost bar, older samples fill
    // leftward, empty leading bars sit at minimum height.
    for (int i = 0; i < n; ++i) {
        const int sample_idx = i - (n - have);
        if (sample_idx < 0) {
            m_strip.setBarHeight(i, 2);
        } else {
            const float v = m_samples[sample_idx];
            const int px = static_cast<int>(v * m_strip.maxHeight());
            m_strip.setBarHeight(i, px);
        }
    }
}

} // namespace lvgl_sim::ui::widgets
