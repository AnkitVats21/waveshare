#pragma once

#include <lvgl.h>

namespace lvgl_sim::ui::widgets {

// A rounded panel with an optional title label and a content body
// container beneath it (a flex-column vertical layout). Callers add their
// own children into body().
class Card {
public:
    void create(lv_obj_t* parent, const char* title, int width);

    lv_obj_t* container() const { return m_container; }
    lv_obj_t* body() const { return m_body; }

    // Makes the whole card clickable (used e.g. for the LED/Assistant rows
    // that open a sheet/screen on tap).
    void setClickable(bool clickable);

private:
    lv_obj_t* m_container = nullptr;
    lv_obj_t* m_title_label = nullptr;
    lv_obj_t* m_body = nullptr;
};

} // namespace lvgl_sim::ui::widgets
