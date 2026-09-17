#include "EqualizerBars.h"

#include <random>

namespace lvgl_sim::ui::widgets {

void EqualizerBars::create(lv_obj_t* parent, int bar_count, int bar_width, int container_height,
                            lv_color_t color_a, lv_color_t color_b) {
    m_strip.create(parent, bar_count, bar_width, container_height, color_a, color_b);

    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> dur_dist(420, 780);

    m_anims.resize(bar_count);
    for (int i = 0; i < bar_count; ++i) {
        lv_anim_t& a = m_anims[i];
        lv_anim_init(&a);
        lv_anim_set_var(&a, m_strip.bar(i));
        lv_anim_set_exec_cb(&a, animCb);
        lv_anim_set_values(&a, 10, 12 + (i % 3) * 20 + 30);
        lv_anim_set_duration(&a, static_cast<uint32_t>(dur_dist(rng)));
        lv_anim_set_playback_duration(&a, static_cast<uint32_t>(dur_dist(rng)));
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    }
    m_strip.setHidden(true);
}

void EqualizerBars::start() {
    if (m_running) return;
    m_running = true;
    m_strip.setHidden(false);
    for (auto& a : m_anims) lv_anim_start(&a);
}

void EqualizerBars::stop() {
    if (!m_running) return;
    m_running = false;
    m_strip.setHidden(true);
    for (int i = 0; i < m_strip.count(); ++i) {
        lv_anim_delete(m_strip.bar(i), animCb);
        m_strip.setBarHeight(i, 12);
    }
}

void EqualizerBars::animCb(void* var, int32_t value) {
    lv_obj_set_height(static_cast<lv_obj_t*>(var), value);
}

} // namespace lvgl_sim::ui::widgets
