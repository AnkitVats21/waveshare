#pragma once

#include "tool_registry/ToolRegistry.h"

namespace Services {

// Registers the voice tools for alarms, timers and reminders (set_alarm,
// set_timer, schedule, list_schedule, cancel_scheduled, ringing_alarm,
// acknowledge_reminders).
void registerAlarmTools(tools::Registry& registry);

} // namespace Services
