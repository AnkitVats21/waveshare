#pragma once

#include "ui_view/UiSnapshot.h"
#include <string>

namespace ui_view {

// Abstraction the UI layer (LVGL screens) codes against. Implementations:
//  - HttpUiDataSource (tools/lvgl_sim): polls the ESP's HTTP API.
//  - SysDbUiDataSource (future, on-device): reads EmbeddedSysDb::snapshot()
//    directly, no network hop.
// Screens must never depend on HTTP or EmbeddedSysDb types directly -- only
// on this interface and UiSnapshot -- so swapping the implementation is the
// only change needed when the real display comes online.
class IUiDataSource {
public:
    virtual ~IUiDataSource() = default;

    virtual UiSnapshot getSnapshot() const = 0;

    // action: "play" | "pause" | "toggle" | "next" | "previous"
    virtual void sendPlaybackAction(const std::string& action) = 0;
    virtual void setVolume(int volume_0_100) = 0;

    // mode: "off" | "solid" | "blink" | "breath" | "rainbow" -- matches
    // POST /api/led/set's body exactly, see HttpFileServerService::ledSetHandler.
    virtual void setLed(const std::string& mode, int r, int g, int b, int speed_ms = 500) = 0;
};

} // namespace ui_view
