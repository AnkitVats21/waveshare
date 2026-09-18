#pragma once

#include "ui_view/IUiDataSource.h"
#include <string>

namespace ui_view {

/**
 * @brief On-device implementation of IUiDataSource.
 *
 * Directly reads state snapshots from EmbeddedSysDb (zero network overhead)
 * and dispatches UI actions (play, pause, next, volume, LED) into firmware services.
 */
class SysDbUiDataSource : public IUiDataSource {
public:
    SysDbUiDataSource();
    ~SysDbUiDataSource() override = default;

    UiSnapshot getSnapshot() const override;

    void sendPlaybackAction(const std::string& action) override;
    void setVolume(int volume_0_100) override;
    void setLed(const std::string& mode, int r, int g, int b, int speed_ms = 500) override;
};

} // namespace ui_view
