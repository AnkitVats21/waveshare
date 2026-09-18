#pragma once

#include "ui_view/UiSnapshot.h"

#include "widgets/EqualizerBars.h"
#include "widgets/TranscriptList.h"

#include <lvgl.h>

#include <functional>

namespace lvgl_sim::ui {

// Screen shown while actively talking to the assistant: a center
// listening/speaking animation (reuses EqualizerBars -- same widget as
// the music equalizer, different color/size instance, per the plan's DRY
// reasoning), a status label, a "Show transcript" switch, and (when
// enabled) a scrolling TranscriptList. Back button returns to Dashboard.
//
// There's no real wake-word/voice-activity event source yet (see the
// plan), so this screen is only reachable by tapping the Dashboard's
// Assistant row -- it doesn't auto-show itself based on snapshot state.
class AssistantScreen {
public:
    AssistantScreen();

    lv_obj_t* root() const { return m_screen; }
    void update(const ui_view::UiSnapshot& snap);

    void setOnBack(std::function<void()> cb) { m_on_back = std::move(cb); }

private:
    lv_obj_t* m_screen = nullptr;
    lv_obj_t* m_status_label = nullptr;
    lv_obj_t* m_transcript_switch = nullptr;
    widgets::EqualizerBars m_anim_bars;
    widgets::TranscriptList m_transcript;

    std::function<void()> m_on_back;

    static void onBackClicked(lv_event_t* e);
    static void onTranscriptSwitchChanged(lv_event_t* e);
};

} // namespace lvgl_sim::ui
