#pragma once

#include <lvgl.h>

#include <string>

namespace lvgl_sim::ui::widgets {

// A labeled horizontal percent bar: a name/value line above a track+fill
// lv_bar. Used for memory, storage, and volume displays.
class HBar {
public:
    void create(lv_obj_t* parent, const char* name, int width);

    // pct in [0, 100]. Fill color is theme::severityColor(pct) unless
    // overridden via setFillColor.
    void setPercent(int pct);
    void setValueText(const std::string& text);
    void setFillColor(lv_color_t color);

    lv_obj_t* container() const { return m_container; }

private:
    lv_obj_t* m_container = nullptr;
    lv_obj_t* m_name_label = nullptr;
    lv_obj_t* m_value_label = nullptr;
    lv_obj_t* m_bar = nullptr;
    bool m_fill_color_overridden = false;
};

} // namespace lvgl_sim::ui::widgets
