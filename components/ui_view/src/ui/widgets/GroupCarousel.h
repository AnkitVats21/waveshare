#pragma once

#include <lvgl.h>

#include <functional>
#include <string>
#include <vector>

namespace lvgl_sim::ui::widgets {

// Wraps lv_tileview: a header (prev/next IconButton + page title + dot
// row) above a horizontally-swipeable set of pages. addPage(title) creates
// one tile and returns its content container for the caller to populate.
class GroupCarousel {
public:
    void create(lv_obj_t* parent, int width, int height);

    // Adds a new page/tile, returns the content container to fill with
    // widgets. Titles are shown in the header when that page is active.
    lv_obj_t* addPage(const std::string& title);

    lv_obj_t* container() const { return m_container; }

private:
    lv_obj_t* m_container = nullptr;
    lv_obj_t* m_tileview = nullptr;
    lv_obj_t* m_title_label = nullptr;
    lv_obj_t* m_dot_row = nullptr;
    lv_obj_t* m_prev_btn = nullptr;
    lv_obj_t* m_next_btn = nullptr;

    std::vector<lv_obj_t*> m_tiles;
    std::vector<std::string> m_titles;
    std::vector<lv_obj_t*> m_dots;
    int m_content_height = 0;

    void goToPage(int index);
    void refreshHeader();
    int currentIndex() const;

    static void onPrevClicked(lv_event_t* e);
    static void onNextClicked(lv_event_t* e);
    static void onTileviewScrollEnd(lv_event_t* e);
};

} // namespace lvgl_sim::ui::widgets
