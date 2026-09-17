#include "IconButton.h"

namespace lvgl_sim::ui::widgets {

lv_obj_t* makeIconButton(lv_obj_t* parent, const char* symbol, int size, lv_color_t bg) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_t* label = lv_label_create(btn);
    lv_obj_center(label);
    lv_label_set_text(label, symbol);
    return btn;
}

} // namespace lvgl_sim::ui::widgets
