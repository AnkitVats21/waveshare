#pragma once

#include "app/gemini_live/gemini_skills_generated.h"
#include <ArduinoJson.h>

class DeviceCommandHandler {
public:
    /**
     * @brief Executes local device commands (Filesystem, LED, Volume, MQTT, Alarms).
     * @param skill_call The decoded skill call details and arguments.
     * @param response_doc The output JSON response to return to Gemini.
     * @return true if handled (supported or safely rejected), false if it should be routed to MPV handler.
     */
    static bool handle(const GeminiSkills::DecodedSkillCall& skill_call, JsonDocument& response_doc);
};
