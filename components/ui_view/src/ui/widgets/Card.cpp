#include "Card.h"

#include "../theme/Theme.h"

namespace lvgl_sim::ui::widgets {

void Card::create(lv_obj_t* parent, const char* title, int width) {
    m_container = lv_obj_create(parent);
    lv_obj_set_size(m_container, width, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(m_container, theme::kCard, 0);
    lv_obj_set_style_border_width(m_container, 0, 0);
    lv_obj_set_style_radius(m_container, 14, 0);
    lv_obj_set_style_pad_all(m_container, 12, 0);
    lv_obj_set_flex_flow(m_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(m_container, 8, 0);

    if (title && title[0] != '\0') {
        m_title_label = lv_label_create(m_container);
        lv_obj_set_style_text_color(m_title_label, theme::kTextMuted, 0);
        lv_label_set_text(m_title_label, title);
    }

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

void Card::setClickable(bool clickable) {
    if (clickable) {
        lv_obj_add_flag(m_container, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(m_container, LV_OBJ_FLAG_CLICKABLE);
    }
}

} // namespace lvgl_sim::ui::widgets
