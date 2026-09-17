#include "MusicPage.h"

#include "IconButton.h"
#include "../theme/Theme.h"

#include <cstdio>

namespace lvgl_sim::ui::widgets {

namespace {

lv_obj_t* makeTextButton(lv_obj_t* parent, const char* text, int height) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_height(btn, height);
    lv_obj_set_width(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(btn, height / 2, 0);
    lv_obj_set_style_bg_color(btn, theme::kCard, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 8, 0);
    lv_obj_t* label = lv_label_create(btn);
    lv_obj_center(label);
    lv_label_set_text(label, text);
    return btn;
}

} // namespace

void MusicPage::create(lv_obj_t* parent, int width, ui_view::IUiDataSource& data_source) {
    m_data_source = &data_source;

    lv_display_t* disp = lv_display_get_default();
    const theme::Metrics metrics = theme::Metrics::forScreen(
        lv_display_get_horizontal_resolution(disp), lv_display_get_vertical_resolution(disp));

    m_title_label = lv_label_create(parent);
    lv_obj_set_width(m_title_label, width);
    lv_label_set_long_mode(m_title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(m_title_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(m_title_label, metrics.font_md, 0);
    lv_label_set_text(m_title_label, "No track playing");

    m_artist_label = lv_label_create(parent);
    lv_obj_set_width(m_artist_label, width);
    lv_label_set_long_mode(m_artist_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(m_artist_label, theme::kTextMuted, 0);
    lv_obj_set_style_text_font(m_artist_label, metrics.font_sm, 0);
    lv_label_set_text(m_artist_label, "--");

    // ---- Mock progress row: bar + time (no buttons -- those live on the
    // transport row below so everything fits on one line) ----
    lv_obj_t* progress_row = lv_obj_create(parent);
    lv_obj_set_size(progress_row, width, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(progress_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(progress_row, 0, 0);
    lv_obj_set_style_pad_all(progress_row, 0, 0);
    lv_obj_set_flex_flow(progress_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(progress_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(progress_row, metrics.spacing_sm, 0);

    m_progress_bar = lv_bar_create(progress_row);
    lv_obj_set_flex_grow(m_progress_bar, 1);
    lv_obj_set_height(m_progress_bar, metrics.bar_thickness);
    lv_obj_set_style_bg_color(m_progress_bar, theme::kCard, LV_PART_MAIN);
    lv_obj_set_style_bg_color(m_progress_bar, theme::kAccent, LV_PART_INDICATOR);
    lv_bar_set_range(m_progress_bar, 0, 100);
    lv_bar_set_value(m_progress_bar, 0, LV_ANIM_OFF);

    m_time_label = lv_label_create(progress_row);
    lv_obj_set_style_text_color(m_time_label, theme::kTextMuted, 0);
    lv_obj_set_style_text_font(m_time_label, metrics.font_sm, 0);
    lv_label_set_text(m_time_label, "0:00");

    // ---- Transport row: -10s / prev / play-pause / next / +10s ----
    lv_obj_t* controls = lv_obj_create(parent);
    lv_obj_set_size(controls, width, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 0, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* seek_back_btn = makeTextButton(controls, "-10s", metrics.icon_btn_sm);
    lv_obj_add_event_cb(seek_back_btn, onSeekBackClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* prev_btn = makeIconButton(controls, LV_SYMBOL_PREV, metrics.icon_btn_sm, theme::kCard);
    lv_obj_add_event_cb(prev_btn, onPrevClicked, LV_EVENT_CLICKED, this);

    m_play_pause_btn = makeIconButton(controls, LV_SYMBOL_PLAY, metrics.icon_btn_md, theme::kAccent);
    lv_obj_add_event_cb(m_play_pause_btn, onPlayPauseClicked, LV_EVENT_CLICKED, this);
    m_play_pause_label = lv_obj_get_child(m_play_pause_btn, 0);

    lv_obj_t* next_btn = makeIconButton(controls, LV_SYMBOL_NEXT, metrics.icon_btn_sm, theme::kCard);
    lv_obj_add_event_cb(next_btn, onNextClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* seek_fwd_btn = makeTextButton(controls, "+10s", metrics.icon_btn_sm);
    lv_obj_add_event_cb(seek_fwd_btn, onSeekForwardClicked, LV_EVENT_CLICKED, this);

    // ---- Volume ----
    lv_obj_t* volume_row = lv_obj_create(parent);
    lv_obj_set_size(volume_row, width, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(volume_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(volume_row, 0, 0);
    lv_obj_set_style_pad_all(volume_row, 0, 0);
    lv_obj_set_flex_flow(volume_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(volume_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(volume_row, metrics.spacing_sm, 0);

    m_volume_icon = lv_label_create(volume_row);
    lv_obj_set_style_text_color(m_volume_icon, theme::kTextMuted, 0);
    lv_obj_set_style_text_font(m_volume_icon, metrics.font_sm, 0);
    lv_label_set_text(m_volume_icon, LV_SYMBOL_VOLUME_MAX);

    m_volume_slider = lv_slider_create(volume_row);
    lv_obj_set_flex_grow(m_volume_slider, 1);
    lv_obj_set_height(m_volume_slider, metrics.bar_thickness + 4);
    lv_obj_set_style_bg_color(m_volume_slider, theme::kAccent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(m_volume_slider, theme::kAccent, LV_PART_KNOB);
    lv_slider_set_range(m_volume_slider, 0, 100);
    // Placeholder -- IUiDataSource::setVolume() is write-only, no read-back
    // field exists, see PlaceholderData.h.
    lv_slider_set_value(m_volume_slider, 80, LV_ANIM_OFF);
    lv_obj_add_event_cb(m_volume_slider, onVolumeChanged, LV_EVENT_RELEASED, this);
}

void MusicPage::refreshTimeLabel() {
    const int elapsed = (m_duration_sec > 0) ? (m_mock_seek_pct * m_duration_sec / 100) : 0;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", elapsed / 60, elapsed % 60);
    lv_label_set_text(m_time_label, buf);
    lv_bar_set_value(m_progress_bar, m_mock_seek_pct, LV_ANIM_OFF);
}

void MusicPage::update(const ui_view::UiSnapshot& snap) {
    const auto& m = snap.music;

    lv_label_set_text(m_title_label, m.title.empty() ? "No track playing" : m.title.c_str());
    lv_label_set_text(m_artist_label, m.artist.empty() ? "--" : m.artist.c_str());

    m_duration_sec = m.duration_sec;
    refreshTimeLabel();

    const bool is_paused = (m.state == ui_view::MusicState::Paused);
    lv_label_set_text(m_play_pause_label, is_paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
}

void MusicPage::onSeekBackClicked(lv_event_t* e) {
    auto* self = static_cast<MusicPage*>(lv_event_get_user_data(e));
    self->m_mock_seek_pct = self->m_mock_seek_pct > 5 ? self->m_mock_seek_pct - 5 : 0;
    self->refreshTimeLabel();
}

void MusicPage::onSeekForwardClicked(lv_event_t* e) {
    auto* self = static_cast<MusicPage*>(lv_event_get_user_data(e));
    self->m_mock_seek_pct = self->m_mock_seek_pct < 95 ? self->m_mock_seek_pct + 5 : 100;
    self->refreshTimeLabel();
}

void MusicPage::onPrevClicked(lv_event_t* e) {
    auto* self = static_cast<MusicPage*>(lv_event_get_user_data(e));
    self->m_data_source->sendPlaybackAction("previous");
}

void MusicPage::onPlayPauseClicked(lv_event_t* e) {
    auto* self = static_cast<MusicPage*>(lv_event_get_user_data(e));
    self->m_data_source->sendPlaybackAction("toggle");
}

void MusicPage::onNextClicked(lv_event_t* e) {
    auto* self = static_cast<MusicPage*>(lv_event_get_user_data(e));
    self->m_data_source->sendPlaybackAction("next");
}

void MusicPage::onVolumeChanged(lv_event_t* e) {
    auto* self = static_cast<MusicPage*>(lv_event_get_user_data(e));
    int val = lv_slider_get_value(self->m_volume_slider);
    self->m_data_source->setVolume(val);
}

} // namespace lvgl_sim::ui::widgets
