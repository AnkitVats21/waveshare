#pragma once

#include "tool_registry/ToolRegistry.h"

namespace Services {

// Registers notes, save_to_memory and set_led_strip.
void registerDeviceTools(tools::Registry& registry);

} // namespace Services
