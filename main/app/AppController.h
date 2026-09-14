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
    bool writeFile(const char* path, const char* content) override;
    std::string readFile(const char* path) override;
    bool fileExists(const char* path) override;
    bool appendFile(const char* path, const char* content) override;
    bool publishMqtt(const char* topic, const char* message) override;
    bool setAlarm(int hour, int minute, const char* tone_file, bool enabled, int& out_alarm_id) override;
    bool stopActiveAlarm() override;

protected:

private:
    AppController();
    ~AppController() override = default;

    static void handleGeminiToolCall(const GeminiSkills::DecodedSkillCall& skill_call, void* ctx);
    void executeToolCall(const GeminiSkills::DecodedSkillCall& skill_call);

    bool m_wifi_connected = false;
    bool m_time_synced = false;

    void initBtCompanion();

    static constexpr const char* TAG = "AppCtrl";
};
