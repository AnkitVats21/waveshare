#pragma once

#include <ArduinoJson.h>
#include "gemini_live/gemini_skills_generated.h"

namespace Services {

// Gemini tools for alarms, timers and reminders (set_alarm, list_alarms,
// cancel_alarm, snooze_alarm, stop_active_alarm, set_timer, set_reminder,
// list_reminders, cancel_reminder, acknowledge_reminders). Fills `response`;
// false if the call is not one of them.
bool handleAlarmTool(const GeminiSkills::DecodedSkillCall& call, JsonDocument& response);

} // namespace Services
