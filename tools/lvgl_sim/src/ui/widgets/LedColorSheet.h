#pragma once

#include <lvgl.h>

#include <functional>
#include <string>
#include <vector>

namespace lvgl_sim::ui::widgets {

// Modal popup: mode chips (Off/Solid/Blink/Breath/Rainbow) + a grid of 8
// preset color swatches (hidden for Off/Rainbow, where color is
// meaningless) + Apply/Cancel. LVGL styles only support 2-stop gradients
// on a single object, so a true rainbow hue slider (as in the original
// HTML mockup) isn't cheaply reproducible here -- this is an explicit
// scope trim from the approved plan. Presets cover the same "tap a
// color, apply" interaction; the mode chips are what actually drive
// POST /api/led/set's animated patterns (solid/blink/breath/rainbow).
class LedColorSheet {
public:
    void create(lv_obj_t* screen_parent);

    void open();
    void close();

    // Keeps the sheet's selection in sync with the device's own read-back
    // state (see DashboardScreen::update()) so it doesn't always reopen
    // defaulted to Solid/purple regardless of what's actually running.
    void setCurrentState(const std::string& mode, lv_color_t color);

    void setOnApply(std::function<void(const std::string& mode, lv_color_t color)> cb) {
        m_on_apply = std::move(cb);
    }

private:
    lv_obj_t* m_overlay = nullptr;
    lv_obj_t* m_sheet = nullptr;

    std::vector<lv_obj_t*> m_mode_chips;
    std::vector<std::string> m_mode_names;
    int m_selected_mode_index = 1; // "solid"

    lv_obj_t* m_color_section = nullptr;
    lv_color_t m_selected_color = lv_color_white();
    std::vector<lv_obj_t*> m_swatches;
    std::vector<lv_color_t> m_colors;

    std::function<void(const std::string& mode, lv_color_t color)> m_on_apply;

    void selectMode(int index);
    void selectSwatch(int index);
    void refreshColorSectionVisibility();

    static void onModeChipClicked(lv_event_t* e);
    static void onSwatchClicked(lv_event_t* e);
    static void onApplyClicked(lv_event_t* e);
    static void onCancelClicked(lv_event_t* e);
};

} // namespace lvgl_sim::ui::widgets
