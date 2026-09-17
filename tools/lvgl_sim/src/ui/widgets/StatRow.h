#pragma once

#include <lvgl.h>

#include <string>

namespace lvgl_sim::ui::widgets {

// icon + label + value header row, with an empty body() slot beneath it
// for a sub-widget (Sparkline/HBar/sub-text/etc). Composition-based: this
// widget itself has no notion of what goes in the body.
class StatRow {
public:
    void create(lv_obj_t* parent, const char* icon_symbol, const char* label, int width);

    lv_obj_t* container() const { return m_container; }
    lv_obj_t* body() const { return m_body; }

    void setValueText(const std::string& text);
    void setValueColor(lv_color_t color);
    void setClickable(bool clickable);

private:
    lv_obj_t* m_container = nullptr;
    lv_obj_t* m_value_label = nullptr;
    lv_obj_t* m_body = nullptr;
};

} // namespace lvgl_sim::ui::widgets
