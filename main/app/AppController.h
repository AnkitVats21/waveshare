#pragma once

#include "core_sysdb/ReactorTask.h"
#include <string>

class AppController : public ReactorTask {
public:
    static AppController& getInstance();

    bool begin();

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

private:
    AppController();
    ~AppController() override = default;

    bool m_wifi_connected = false;

    static constexpr const char* TAG = "AppCtrl";
};
