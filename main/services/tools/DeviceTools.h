#pragma once

#include "tool_registry/ToolRegistry.h"

namespace Services {

// Registers notes, save_to_memory, set_led_strip, music_status and device_info.
void registerDeviceTools(tools::Registry& registry);

} // namespace Services
