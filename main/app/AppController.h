#pragma once

#include "common/ReactorTask.h"
#include "gemini_live/gemini_skills_generated.h"
#include "gemini_live/IDeviceCommandDelegate.h"
#include <string>

class AppController : public ReactorTask, public IDeviceCommandDelegate {
public:
    static AppController& getInstance();

    bool begin();

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

    // IDeviceCommandDelegate interface
    bool handleAlarmTool(const GeminiSkills::DecodedSkillCall& call, JsonDocument& response) override;

protected:

private:
    AppController();
    ~AppController() override = default;

    static void handleGeminiToolCall(const GeminiSkills::DecodedSkillCall& skill_call, void* ctx);
    void executeToolCall(const GeminiSkills::DecodedSkillCall& skill_call);

    bool m_wifi_connected = false;


    static constexpr const char* TAG = "AppCtrl";
};
