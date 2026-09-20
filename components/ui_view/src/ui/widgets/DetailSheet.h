#pragma once

#include <lvgl.h>

#include <string>

namespace lvgl_sim::ui::widgets {

// Generic modal popup: title + a body text line + a single Close button.
// One shared instance is reused for every topbar status icon (Wi-Fi, ...)
// since only one can be open at a time -- show() just replaces the
// title/body text and opens it. Mirrors LedColorSheet's overlay/sheet
// mechanics, minus the swatch grid.
class DetailSheet {
public:
    void create(lv_obj_t* screen_parent);

    void show(const std::string& title, const std::string& body);
    void close();

private:
    lv_obj_t* m_overlay = nullptr;
    lv_obj_t* m_sheet = nullptr;
    lv_obj_t* m_title_label = nullptr;
    lv_obj_t* m_body_label = nullptr;

    static void onCloseClicked(lv_event_t* e);
};

} // namespace lvgl_sim::ui::widgets
