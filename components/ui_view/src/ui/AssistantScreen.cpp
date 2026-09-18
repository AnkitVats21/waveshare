#include "AssistantScreen.h"

#include "PlaceholderData.h"
#include "theme/Theme.h"
#include "widgets/IconButton.h"

namespace lvgl_sim::ui {

using widgets::makeIconButton;
namespace theme_ns = theme;

AssistantScreen::AssistantScreen() {
    const int w = lv_display_get_horizontal_resolution(nullptr);
    const int h = lv_display_get_vertical_resolution(nullptr);
    const theme_ns::Metrics metrics = theme_ns::Metrics::forScreen(w, h);

    m_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(m_screen, theme_ns::kBg, 0);
    lv_obj_set_style_pad_all(m_screen, metrics.spacing_lg, 0);
    lv_obj_set_flex_flow(m_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m_screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(m_screen, metrics.spacing_md, 0);

    lv_obj_t* back_btn = makeIconButton(m_screen, LV_SYMBOL_LEFT, metrics.icon_btn_md, theme_ns::kCard);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, metrics.spacing_sm, metrics.spacing_sm);
    lv_obj_add_event_cb(back_btn, onBackClicked, LV_EVENT_CLICKED, this);

    // Reuses the same "big animated visual centerpiece" size token as
    // NowPlayingScreen's album art, rather than a second hardcoded literal.
    const int anim_w = metrics.now_playing_art_size;
    const int anim_h = anim_w * 2 / 5;

    lv_obj_t* anim_container = lv_obj_create(m_screen);
    lv_obj_set_size(anim_container, anim_w, anim_h);
    lv_obj_set_style_bg_opa(anim_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(anim_container, 0, 0);
    lv_obj_set_style_pad_all(anim_container, 0, 0);
    m_anim_bars.create(anim_container, 7, anim_w / 16, anim_h, theme_ns::kAccent, theme_ns::kAccent2);
    m_anim_bars.start();

    m_status_label = lv_label_create(m_screen);
    lv_obj_set_style_text_color(m_status_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(m_status_label, metrics.font_xl, 0);
    lv_label_set_text(m_status_label, "Listening...");

    lv_obj_t* switch_row = lv_obj_create(m_screen);
    lv_obj_set_size(switch_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(switch_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(switch_row, 0, 0);
    lv_obj_set_style_pad_all(switch_row, 0, 0);
    lv_obj_set_flex_flow(switch_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(switch_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(switch_row, 10, 0);

    lv_obj_t* switch_label = lv_label_create(switch_row);
    lv_obj_set_style_text_color(switch_label, theme_ns::kTextMuted, 0);
    lv_label_set_text(switch_label, "Show transcript");

    m_transcript_switch = lv_switch_create(switch_row);
    lv_obj_set_style_bg_color(m_transcript_switch, theme_ns::kAccent, static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | LV_STATE_CHECKED);
    lv_obj_add_event_cb(m_transcript_switch, onTranscriptSwitchChanged, LV_EVENT_VALUE_CHANGED, this);

    const int transcript_width = (w > 400) ? 500 : (w - 2 * metrics.spacing_lg);
    const int transcript_height = (h > 400) ? 220 : 90;
    m_transcript.create(m_screen, transcript_width, transcript_height);
    m_transcript.setHidden(true);
    for (const auto& entry : placeholder::sampleTranscript()) {
        m_transcript.addEntry(entry.speaker, entry.text);
    }
}

void AssistantScreen::update(const ui_view::UiSnapshot& snap) {
    (void)snap; // No live assistant-state field in UiSnapshot yet -- status
                // stays on its placeholder "Listening..." text; a real
                // implementation would switch between
                // Listening/Thinking/Speaking off a future snapshot field.
}

void AssistantScreen::onBackClicked(lv_event_t* e) {
    auto* self = static_cast<AssistantScreen*>(lv_event_get_user_data(e));
    if (self->m_on_back) self->m_on_back();
}

void AssistantScreen::onTranscriptSwitchChanged(lv_event_t* e) {
    auto* self = static_cast<AssistantScreen*>(lv_event_get_user_data(e));
    const bool checked = lv_obj_has_state(self->m_transcript_switch, LV_STATE_CHECKED);
    self->m_transcript.setHidden(!checked);
}

} // namespace lvgl_sim::ui
