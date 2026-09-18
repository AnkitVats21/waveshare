#include "BarStrip.h"

namespace lvgl_sim::ui::widgets {

void BarStrip::create(lv_obj_t* parent, int count, int bar_width, int container_height,
                       lv_color_t color_a, lv_color_t color_b) {
    m_container_height = container_height;

    m_row = lv_obj_create(parent);
    lv_obj_set_size(m_row, LV_SIZE_CONTENT, container_height);
    lv_obj_set_style_bg_opa(m_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_row, 0, 0);
    lv_obj_set_style_pad_all(m_row, 0, 0);
    lv_obj_set_flex_flow(m_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(m_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_scrollbar_mode(m_row, LV_SCROLLBAR_MODE_OFF);

    m_bars.resize(count);
    for (int i = 0; i < count; ++i) {
        lv_obj_t* bar = lv_obj_create(m_row);
        lv_obj_set_size(bar, bar_width, 4);
        lv_obj_set_style_radius(bar, bar_width / 2, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_bg_color(bar, (i % 2 == 0) ? color_a : color_b, 0);
        m_bars[i] = bar;
    }
}

void BarStrip::setBarHeight(int i, int px) {
    if (i < 0 || i >= static_cast<int>(m_bars.size())) return;
    if (px < 2) px = 2;
    if (px > m_container_height) px = m_container_height;
    lv_obj_set_height(m_bars[i], px);
}

void BarStrip::setHidden(bool hidden) {
    if (hidden) {
        lv_obj_add_flag(m_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(m_row, LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace lvgl_sim::ui::widgets
