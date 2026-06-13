#pragma once

#include "app/gemini_live/gemini_skills_generated.h"
#include <ArduinoJson.h>

class MpvCommandHandler {
public:
    /**
     * @brief Translates Gemini tool calls into MQTT messages published to HiveMQ mpv/command topic.
     * @param skill_call The decoded skill call details and arguments.
     * @param response_doc The output JSON response to return to Gemini.
     * @return true if handled, false if unsupported.
     */
    static bool handle(const GeminiSkills::DecodedSkillCall& skill_call, JsonDocument& response_doc);
};
