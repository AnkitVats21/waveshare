#include "LedColorSheet.h"

#include "../theme/Theme.h"
#include "IconButton.h"

namespace lvgl_sim::ui::widgets {

namespace {
// Index into these two parallel vectors is packed into the swatch's user
// data as an intptr_t.
constexpr lv_color_t kPresets[8] = {
    LV_COLOR_MAKE(0x7c, 0x4d, 0xff), // accent purple
    LV_COLOR_MAKE(0x00, 0xd9, 0xc0), // accent teal
    LV_COLOR_MAKE(0xe7, 0x4c, 0x3c), // red
    LV_COLOR_MAKE(0xf5, 0xa6, 0x23), // amber
    LV_COLOR_MAKE(0x2e, 0xcc, 0x71), // green
    LV_COLOR_MAKE(0x34, 0x98, 0xdb), // blue
    LV_COLOR_MAKE(0xff, 0x66, 0xcc), // pink
    LV_COLOR_MAKE(0xff, 0xff, 0xff), // white
};

// Order matches the on-device LedMode enum (OFF, SOLID, BLINK, BREATH,
// RAINBOW) and the lowercase strings POST /api/led/set accepts.
constexpr const char* kModeNames[] = {"off", "solid", "blink", "breath", "rainbow"};
constexpr const char* kModeLabels[] = {"Off", "Solid", "Blink", "Breath", "Rainbow"};

// Color is meaningless for Off (strip is dark) and Rainbow (cycles all
// hues on its own) -- the swatch grid only matters for the other three.
bool modeUsesColor(int index) { return index == 1 || index == 2 || index == 3; }

lv_obj_t* makeChip(lv_obj_t* parent, const char* text) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_height(btn, 32);
    lv_obj_set_width(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_bg_color(btn, theme::kBg, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, theme::kTextMuted, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 10, 0);
    lv_obj_t* label = lv_label_create(btn);
    lv_obj_center(label);
    lv_label_set_text(label, text);
    return btn;
}

} // namespace

void LedColorSheet::create(lv_obj_t* screen_parent) {
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
    lv_obj_set_size(m_sheet, LV_PCT(85), LV_SIZE_CONTENT);
    lv_obj_center(m_sheet);
    lv_obj_set_style_bg_color(m_sheet, theme::kCard, 0);
    lv_obj_set_style_border_width(m_sheet, 0, 0);
    lv_obj_set_style_radius(m_sheet, 16, 0);
    lv_obj_set_style_pad_all(m_sheet, 16, 0);
    lv_obj_set_flex_flow(m_sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m_sheet, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(m_sheet, 12, 0);

    lv_obj_t* title = lv_label_create(m_sheet);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "LED");

    // ---- Mode chips ----
    lv_obj_t* mode_row = lv_obj_create(m_sheet);
    lv_obj_set_size(mode_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(mode_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mode_row, 0, 0);
    lv_obj_set_style_pad_all(mode_row, 0, 0);
    lv_obj_set_flex_flow(mode_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(mode_row, 8, 0);
    lv_obj_set_style_pad_row(mode_row, 8, 0);
    lv_obj_set_width(mode_row, 4 * (60 + 8));
    lv_obj_set_flex_align(mode_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < 5; ++i) {
        m_mode_names.push_back(kModeNames[i]);
        lv_obj_t* chip = makeChip(mode_row, kModeLabels[i]);
        lv_obj_add_event_cb(chip, onModeChipClicked, LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(chip, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        m_mode_chips.push_back(chip);
    }

    // ---- Color swatch grid (hidden for Off/Rainbow) ----
    m_color_section = lv_obj_create(m_sheet);
    lv_obj_set_size(m_color_section, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(m_color_section, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_color_section, 0, 0);
    lv_obj_set_style_pad_all(m_color_section, 0, 0);
    lv_obj_set_flex_flow(m_color_section, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(m_color_section, 10, 0);
    lv_obj_set_style_pad_row(m_color_section, 10, 0);
    lv_obj_set_width(m_color_section, 4 * (28 + 10));

    for (int i = 0; i < 8; ++i) {
        m_colors.push_back(kPresets[i]);
        lv_obj_t* swatch = lv_obj_create(m_color_section);
        lv_obj_set_size(swatch, 28, 28);
        lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(swatch, kPresets[i], 0);
        lv_obj_set_style_border_width(swatch, 2, 0);
        lv_obj_set_style_border_color(swatch, theme::kTextMuted, 0);
        lv_obj_add_flag(swatch, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(swatch, onSwatchClicked, LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(swatch, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        m_swatches.push_back(swatch);
    }
    m_selected_color = kPresets[0];

    lv_obj_t* btn_row = lv_obj_create(m_sheet);
    lv_obj_set_size(btn_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 12, 0);

    lv_obj_t* cancel_btn = lv_button_create(btn_row);
    lv_obj_set_style_bg_color(cancel_btn, theme::kBg, 0);
    lv_obj_t* cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_add_event_cb(cancel_btn, onCancelClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* apply_btn = lv_button_create(btn_row);
    lv_obj_set_style_bg_color(apply_btn, theme::kAccent, 0);
    lv_obj_t* apply_label = lv_label_create(apply_btn);
    lv_label_set_text(apply_label, "Apply");
    lv_obj_add_event_cb(apply_btn, onApplyClicked, LV_EVENT_CLICKED, this);

    selectMode(m_selected_mode_index);
    selectSwatch(0);
}

void LedColorSheet::open() {
    lv_obj_remove_flag(m_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(m_overlay);
}

void LedColorSheet::close() {
    lv_obj_add_flag(m_overlay, LV_OBJ_FLAG_HIDDEN);
}

void LedColorSheet::setCurrentState(const std::string& mode, lv_color_t color) {
    for (size_t i = 0; i < m_mode_names.size(); ++i) {
        if (m_mode_names[i] == mode) {
            selectMode(static_cast<int>(i));
            break;
        }
    }
    m_selected_color = color;
    for (size_t i = 0; i < m_swatches.size(); ++i) {
        const bool match = m_colors[i].red == color.red && m_colors[i].green == color.green &&
                            m_colors[i].blue == color.blue;
        lv_obj_set_style_border_color(m_swatches[i], match ? lv_color_white() : theme::kTextMuted, 0);
        lv_obj_set_style_border_width(m_swatches[i], match ? 3 : 2, 0);
    }
}

void LedColorSheet::selectMode(int index) {
    if (index < 0 || index >= static_cast<int>(m_mode_chips.size())) return;
    m_selected_mode_index = index;
    for (size_t i = 0; i < m_mode_chips.size(); ++i) {
        const bool selected = (static_cast<int>(i) == index);
        lv_obj_set_style_bg_color(m_mode_chips[i], selected ? theme::kAccent : theme::kBg, 0);
        lv_obj_set_style_border_color(m_mode_chips[i], selected ? theme::kAccent : theme::kTextMuted, 0);
    }
    refreshColorSectionVisibility();
}

void LedColorSheet::selectSwatch(int index) {
    if (index < 0 || index >= static_cast<int>(m_colors.size())) return;
    m_selected_color = m_colors[index];
    for (size_t i = 0; i < m_swatches.size(); ++i) {
        lv_obj_set_style_border_color(m_swatches[i],
            (static_cast<int>(i) == index) ? lv_color_white() : theme::kTextMuted, 0);
        lv_obj_set_style_border_width(m_swatches[i], (static_cast<int>(i) == index) ? 3 : 2, 0);
    }
}

void LedColorSheet::refreshColorSectionVisibility() {
    if (modeUsesColor(m_selected_mode_index)) {
        lv_obj_remove_flag(m_color_section, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(m_color_section, LV_OBJ_FLAG_HIDDEN);
    }
}

void LedColorSheet::onModeChipClicked(lv_event_t* e) {
    auto* self = static_cast<LedColorSheet*>(lv_event_get_user_data(e));
    lv_obj_t* chip = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(chip)));
    self->selectMode(index);
}

void LedColorSheet::onSwatchClicked(lv_event_t* e) {
    auto* self = static_cast<LedColorSheet*>(lv_event_get_user_data(e));
    lv_obj_t* swatch = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(swatch)));
    self->selectSwatch(index);
}

void LedColorSheet::onApplyClicked(lv_event_t* e) {
    auto* self = static_cast<LedColorSheet*>(lv_event_get_user_data(e));
    if (self->m_on_apply) {
        self->m_on_apply(self->m_mode_names[self->m_selected_mode_index], self->m_selected_color);
    }
    self->close();
}

void LedColorSheet::onCancelClicked(lv_event_t* e) {
    auto* self = static_cast<LedColorSheet*>(lv_event_get_user_data(e));
    self->close();
}

} // namespace lvgl_sim::ui::widgets
