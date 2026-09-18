#include "StatRow.h"

#include "../theme/Theme.h"

namespace lvgl_sim::ui::widgets {

void StatRow::create(lv_obj_t* parent, const char* icon_symbol, const char* label, int width) {
    m_container = lv_obj_create(parent);
    lv_obj_set_size(m_container, width, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(m_container, theme::kCard, 0);
    lv_obj_set_style_border_width(m_container, 0, 0);
    lv_obj_set_style_radius(m_container, 14, 0);
    lv_obj_set_style_pad_all(m_container, 12, 0);
    lv_obj_set_flex_flow(m_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(m_container, 6, 0);

    lv_obj_t* header = lv_obj_create(m_container);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* left = lv_obj_create(header);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 8, 0);

    if (icon_symbol && icon_symbol[0] != '\0') {
        lv_obj_t* icon = lv_label_create(left);
        lv_obj_set_style_text_color(icon, theme::kAccent2, 0);
        lv_label_set_text(icon, icon_symbol);
    }

    lv_obj_t* label_widget = lv_label_create(left);
    lv_obj_set_style_text_color(label_widget, lv_color_white(), 0);
    lv_label_set_text(label_widget, label);

    m_value_label = lv_label_create(header);
    lv_obj_set_style_text_color(m_value_label, theme::kTextMuted, 0);
    lv_label_set_text(m_value_label, "--");

    m_body = lv_obj_create(m_container);
    lv_obj_set_size(m_body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(m_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_body, 0, 0);
    lv_obj_set_style_pad_all(m_body, 0, 0);
    lv_obj_set_flex_flow(m_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(m_body, 6, 0);
    lv_obj_set_scrollbar_mode(m_body, LV_SCROLLBAR_MODE_OFF);
}

void StatRow::setValueText(const std::string& text) {
    lv_label_set_text(m_value_label, text.c_str());
}

void StatRow::setValueColor(lv_color_t color) {
    lv_obj_set_style_text_color(m_value_label, color, 0);
}

void StatRow::setClickable(bool clickable) {
    if (clickable) {
        lv_obj_add_flag(m_container, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(m_container, LV_OBJ_FLAG_CLICKABLE);
    }
}

} // namespace lvgl_sim::ui::widgets
