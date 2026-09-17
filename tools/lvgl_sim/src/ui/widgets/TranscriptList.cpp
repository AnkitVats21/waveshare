#include "TranscriptList.h"

#include "../theme/Theme.h"

namespace lvgl_sim::ui::widgets {

void TranscriptList::create(lv_obj_t* parent, int width, int height) {
    m_container = lv_obj_create(parent);
    lv_obj_set_size(m_container, width, height);
    lv_obj_set_style_bg_color(m_container, theme::kCard, 0);
    lv_obj_set_style_border_width(m_container, 0, 0);
    lv_obj_set_style_radius(m_container, 12, 0);
    lv_obj_set_style_pad_all(m_container, 10, 0);
    lv_obj_set_flex_flow(m_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(m_container, 8, 0);
    lv_obj_set_scroll_dir(m_container, LV_DIR_VER);
}

void TranscriptList::clear() {
    lv_obj_clean(m_container);
}

void TranscriptList::addEntry(const std::string& speaker, const std::string& text) {
    lv_obj_t* row = lv_obj_create(m_container);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    const bool is_user = (speaker == "You");
    lv_obj_t* speaker_label = lv_label_create(row);
    lv_obj_set_style_text_color(speaker_label, is_user ? theme::kAccent2 : theme::kAccent, 0);
    lv_obj_set_style_text_font(speaker_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(speaker_label, speaker.c_str());

    lv_obj_t* text_label = lv_label_create(row);
    lv_obj_set_width(text_label, LV_PCT(100));
    lv_label_set_long_mode(text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(text_label, lv_color_white(), 0);
    lv_label_set_text(text_label, text.c_str());

    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
}

void TranscriptList::setHidden(bool hidden) {
    if (hidden) {
        lv_obj_add_flag(m_container, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(m_container, LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace lvgl_sim::ui::widgets
