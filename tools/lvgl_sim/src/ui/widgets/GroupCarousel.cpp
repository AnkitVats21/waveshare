#include "GroupCarousel.h"

#include "../theme/Theme.h"
#include "IconButton.h"

namespace lvgl_sim::ui::widgets {

void GroupCarousel::create(lv_obj_t* parent, int width, int height) {
    m_container = lv_obj_create(parent);
    lv_obj_set_size(m_container, width, height);
    lv_obj_set_style_bg_opa(m_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_container, 0, 0);
    lv_obj_set_style_pad_all(m_container, 0, 0);
    lv_obj_set_flex_flow(m_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(m_container, 4, 0);

    // ---- Header: prev arrow / title / next arrow, dot row below ----
    lv_obj_t* header = lv_obj_create(m_container);
    lv_obj_set_size(header, width, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    m_prev_btn = makeIconButton(header, LV_SYMBOL_LEFT, 24, theme::kCard);
    lv_obj_add_event_cb(m_prev_btn, onPrevClicked, LV_EVENT_CLICKED, this);

    m_title_label = lv_label_create(header);
    lv_obj_set_style_text_color(m_title_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(m_title_label, &lv_font_montserrat_18, 0);
    lv_label_set_text(m_title_label, "");

    m_next_btn = makeIconButton(header, LV_SYMBOL_RIGHT, 24, theme::kCard);
    lv_obj_add_event_cb(m_next_btn, onNextClicked, LV_EVENT_CLICKED, this);

    m_dot_row = lv_obj_create(m_container);
    lv_obj_set_size(m_dot_row, width, 10);
    lv_obj_set_style_bg_opa(m_dot_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_dot_row, 0, 0);
    lv_obj_set_style_pad_all(m_dot_row, 0, 0);
    lv_obj_set_flex_flow(m_dot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(m_dot_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(m_dot_row, 6, 0);

    // ---- Tileview body ----
    m_content_height = height - 48;
    if (m_content_height < 40) m_content_height = 40;

    m_tileview = lv_tileview_create(m_container);
    lv_obj_set_size(m_tileview, width, m_content_height);
    lv_obj_set_style_bg_opa(m_tileview, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_tileview, 0, 0);
    lv_obj_add_event_cb(m_tileview, onTileviewScrollEnd, LV_EVENT_SCROLL_END, this);
}

lv_obj_t* GroupCarousel::addPage(const std::string& title) {
    const int index = static_cast<int>(m_tiles.size());
    lv_obj_t* tile = lv_tileview_add_tile(m_tileview, index, 0, LV_DIR_HOR);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(tile, 10, 0);
    lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);

    m_tiles.push_back(tile);
    m_titles.push_back(title);

    lv_obj_t* dot = lv_obj_create(m_dot_row);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_bg_color(dot, theme::kTextMuted, 0);
    m_dots.push_back(dot);

    if (index == 0) refreshHeader();
    return tile;
}

int GroupCarousel::currentIndex() const {
    if (m_tiles.empty()) return 0;
    lv_obj_t* act = lv_tileview_get_tile_active(m_tileview);
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        if (m_tiles[i] == act) return static_cast<int>(i);
    }
    return 0;
}

void GroupCarousel::goToPage(int index) {
    if (m_tiles.empty()) return;
    if (index < 0) index = 0;
    if (index >= static_cast<int>(m_tiles.size())) index = static_cast<int>(m_tiles.size()) - 1;
    lv_obj_set_tile(m_tileview, m_tiles[index], LV_ANIM_ON);
    refreshHeader();
}

void GroupCarousel::refreshHeader() {
    const int idx = currentIndex();
    if (idx >= 0 && idx < static_cast<int>(m_titles.size())) {
        lv_label_set_text(m_title_label, m_titles[idx].c_str());
    }
    for (size_t i = 0; i < m_dots.size(); ++i) {
        lv_obj_set_style_bg_color(m_dots[i],
            (static_cast<int>(i) == idx) ? theme::kAccent2 : theme::kTextMuted, 0);
    }
}

void GroupCarousel::onPrevClicked(lv_event_t* e) {
    auto* self = static_cast<GroupCarousel*>(lv_event_get_user_data(e));
    self->goToPage(self->currentIndex() - 1);
}

void GroupCarousel::onNextClicked(lv_event_t* e) {
    auto* self = static_cast<GroupCarousel*>(lv_event_get_user_data(e));
    self->goToPage(self->currentIndex() + 1);
}

void GroupCarousel::onTileviewScrollEnd(lv_event_t* e) {
    auto* self = static_cast<GroupCarousel*>(lv_event_get_user_data(e));
    self->refreshHeader();
}

} // namespace lvgl_sim::ui::widgets
