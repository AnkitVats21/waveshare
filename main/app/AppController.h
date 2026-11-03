#pragma once

#include "core_sysdb/ReactorTask.h"
#include "tool_registry/ToolRegistry.h"
#include <string>

class AppController : public ReactorTask {
public:
    static AppController& getInstance();

    bool begin(tools::Registry& tools);

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

private:
    AppController();
    ~AppController() override = default;

    // A music tool answered: the session ends once the reply is spoken.
    static void onToolEvent(const tools::Event& event, void* ctx);

    bool m_wifi_connected = false;

    static constexpr const char* TAG = "AppCtrl";
};
