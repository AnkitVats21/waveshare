#pragma once

#include <ArduinoJson.h>
#include "gemini_live/gemini_skills_generated.h"

/**
 * @brief Abstract interface for platform/board specific device command execution.
 * Decouples Gemini tool dispatching from board services (the alarm scheduler).
 * File tools use sd_storage directly.
 */
class IDeviceCommandDelegate {
public:
    virtual ~IDeviceCommandDelegate() = default;

    // Alarm, timer and reminder tools. False if the call is not one of them.
    virtual bool handleAlarmTool(const GeminiSkills::DecodedSkillCall& call, JsonDocument& response) = 0;
};
