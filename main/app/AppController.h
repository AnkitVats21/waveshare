#pragma once

#include "common/ReactorTask.h"
#include "gemini_skills_generated.h"

class AppController : public ReactorTask {
public:
    static AppController& getInstance();

    bool begin();

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:

private:
    AppController();
    ~AppController() override = default;

    static void handleGeminiToolCall(const GeminiSkills::DecodedSkillCall& skill_call, void* ctx);
    void executeToolCall(const GeminiSkills::DecodedSkillCall& skill_call);

    bool m_wifi_connected = false;
    bool m_time_synced = false;

    static constexpr const char* TAG = "AppCtrl";
};
