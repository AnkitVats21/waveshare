#pragma once

#include <lvgl.h>

namespace lvgl_sim::ui::widgets {

// A circular icon button (symbol glyph centered in a colored circle).
// Factored out of the original NowPlayingScreen::makeIconButton, used by
// transport controls, carousel arrows, sheet close buttons, and topbar
// back buttons. Returns the button object; the caller attaches its own
// LV_EVENT_CLICKED handler.
lv_obj_t* makeIconButton(lv_obj_t* parent, const char* symbol, int size, lv_color_t bg);

} // namespace lvgl_sim::ui::widgets
