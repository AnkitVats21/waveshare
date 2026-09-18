#include "HBar.h"

#include "../theme/Theme.h"

#include <cstdio>

namespace lvgl_sim::ui::widgets {

void HBar::create(lv_obj_t* parent, const char* name, int width) {
    m_container = lv_obj_create(parent);
    lv_obj_set_size(m_container, width, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(m_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_container, 0, 0);
    lv_obj_set_style_pad_all(m_container, 0, 0);
    lv_obj_set_flex_flow(m_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(m_container, 4, 0);

    lv_obj_t* label_row = lv_obj_create(m_container);
    lv_obj_set_size(label_row, width, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(label_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(label_row, 0, 0);
    lv_obj_set_style_pad_all(label_row, 0, 0);
    lv_obj_set_flex_flow(label_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(label_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    m_name_label = lv_label_create(label_row);
    lv_obj_set_style_text_color(m_name_label, theme::kTextMuted, 0);
    lv_label_set_text(m_name_label, name);

    m_value_label = lv_label_create(label_row);
    lv_obj_set_style_text_color(m_value_label, lv_color_white(), 0);
    lv_label_set_text(m_value_label, "--");

    m_bar = lv_bar_create(m_container);
    lv_obj_set_size(m_bar, width, 8);
    lv_obj_set_style_bg_color(m_bar, theme::kCard, LV_PART_MAIN);
    lv_obj_set_style_bg_color(m_bar, theme::kGood, LV_PART_INDICATOR);
    lv_bar_set_range(m_bar, 0, 100);
    lv_bar_set_value(m_bar, 0, LV_ANIM_OFF);
}

void HBar::setPercent(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_bar_set_value(m_bar, pct, LV_ANIM_OFF);
    if (!m_fill_color_overridden) {
        lv_obj_set_style_bg_color(m_bar, theme::severityColor(pct), LV_PART_INDICATOR);
    }
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(m_value_label, buf);
}

void HBar::setValueText(const std::string& text) {
    lv_label_set_text(m_value_label, text.c_str());
}

void HBar::setFillColor(lv_color_t color) {
    m_fill_color_overridden = true;
    lv_obj_set_style_bg_color(m_bar, color, LV_PART_INDICATOR);
}

} // namespace lvgl_sim::ui::widgets
