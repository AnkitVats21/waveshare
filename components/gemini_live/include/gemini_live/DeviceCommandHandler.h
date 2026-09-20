#pragma once

#include "gemini_skills_generated.h"
#include "IDeviceCommandDelegate.h"
#include <ArduinoJson.h>

class DeviceCommandHandler {
public:
    /**
     * @brief Injects the platform/board delegate to handle hardware-specific actions.
     */
    static void setDelegate(IDeviceCommandDelegate* delegate);

    /**
     * @brief Gets current delegate (or nullptr if none registered).
     */
    static IDeviceCommandDelegate* getDelegate();

    /**
     * @brief Executes local device commands (Filesystem, LED, Volume, Alarms).
     * @param skill_call The decoded skill call details and arguments.
     * @param response_doc The output JSON response to return to Gemini.
     * @return true if handled (supported or safely rejected), false if it should be routed to Media handler.
     */
    static bool handle(const GeminiSkills::DecodedSkillCall& skill_call, JsonDocument& response_doc);

private:
    static IDeviceCommandDelegate* s_delegate;
};
