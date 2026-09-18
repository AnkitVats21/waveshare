#pragma once

#include <lvgl.h>

#include <vector>

namespace lvgl_sim::ui::widgets {

// Low-level primitive: a row of N thin rounded bars whose individual
// heights can be set directly. Both EqualizerBars (animation-driven) and
// Sparkline (data-driven) are built on top of this -- it owns only the bar
// objects and their layout, no animation or data logic of its own.
class BarStrip {
public:
    // Creates `count` bars inside a new flex-row container placed in
    // `parent`, each `bar_width` wide, laid out to fill `container_height`
    // tall, alternating between `color_a`/`color_b`.
    void create(lv_obj_t* parent, int count, int bar_width, int container_height,
                lv_color_t color_a, lv_color_t color_b);

    lv_obj_t* row() const { return m_row; }
    int count() const { return static_cast<int>(m_bars.size()); }
    int maxHeight() const { return m_container_height; }

    lv_obj_t* bar(int i) const { return m_bars[i]; }
    void setBarHeight(int i, int px);

    void setHidden(bool hidden);

private:
    lv_obj_t* m_row = nullptr;
    std::vector<lv_obj_t*> m_bars;
    int m_container_height = 0;
};

} // namespace lvgl_sim::ui::widgets
