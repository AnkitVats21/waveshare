#pragma once

#include <lvgl.h>

#include <string>

namespace lvgl_sim::ui::widgets {

// A scrollable column of speaker/text rows, e.g. for a live conversation
// transcript. Reusable outside AssistantScreen if a transcript view is
// ever needed elsewhere.
class TranscriptList {
public:
    void create(lv_obj_t* parent, int width, int height);

    void clear();
    void addEntry(const std::string& speaker, const std::string& text);

    lv_obj_t* container() const { return m_container; }
    void setHidden(bool hidden);

private:
    lv_obj_t* m_container = nullptr;
};

} // namespace lvgl_sim::ui::widgets
