#include "DetailSheet.h"

#include "../theme/Theme.h"

namespace lvgl_sim::ui::widgets {

void DetailSheet::create(lv_obj_t* screen_parent) {
    m_overlay = lv_obj_create(screen_parent);
    lv_obj_set_size(m_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(m_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(m_overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(m_overlay, 0, 0);
    lv_obj_set_style_radius(m_overlay, 0, 0);
    lv_obj_set_style_pad_all(m_overlay, 0, 0);
    lv_obj_add_flag(m_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(m_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(m_overlay);

    m_sheet = lv_obj_create(m_overlay);
    lv_obj_set_size(m_sheet, LV_PCT(80), LV_SIZE_CONTENT);
    lv_obj_center(m_sheet);
    lv_obj_set_style_bg_color(m_sheet, theme::kCard, 0);
    lv_obj_set_style_border_width(m_sheet, 0, 0);
    lv_obj_set_style_radius(m_sheet, 16, 0);
    lv_obj_set_style_pad_all(m_sheet, 16, 0);
    lv_obj_set_flex_flow(m_sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m_sheet, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(m_sheet, 10, 0);

    m_title_label = lv_label_create(m_sheet);
    lv_obj_set_style_text_color(m_title_label, lv_color_white(), 0);

    m_body_label = lv_label_create(m_sheet);
    lv_obj_set_style_text_color(m_body_label, theme::kTextMuted, 0);
    lv_label_set_long_mode(m_body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m_body_label, LV_PCT(100));

    lv_obj_t* close_btn = lv_button_create(m_sheet);
    lv_obj_set_style_bg_color(close_btn, theme::kAccent, 0);
    lv_obj_t* close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "Close");
    lv_obj_add_event_cb(close_btn, onCloseClicked, LV_EVENT_CLICKED, this);
}

void DetailSheet::show(const std::string& title, const std::string& body) {
    lv_label_set_text(m_title_label, title.c_str());
    lv_label_set_text(m_body_label, body.c_str());
    lv_obj_remove_flag(m_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(m_overlay);
}

void DetailSheet::close() {
    lv_obj_add_flag(m_overlay, LV_OBJ_FLAG_HIDDEN);
}

void DetailSheet::onCloseClicked(lv_event_t* e) {
    auto* self = static_cast<DetailSheet*>(lv_event_get_user_data(e));
    self->close();
}

} // namespace lvgl_sim::ui::widgets
