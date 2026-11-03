#pragma once
// get_weather: current conditions and a daily forecast from Open-Meteo
// (keyless). The fetch (geocoding, then the forecast; ~1-2 s) runs on a
// short-lived worker task, which sends the tool response itself, so the
// Gemini protocol task keeps draining reply audio meanwhile.

#include "tool_registry/ToolRegistry.h"

namespace Services::Weather {

// Registers get_weather: the lookup for `location` (empty: the
// weather_location setting) with `days` of forecast (clamped to 1-7).
void registerTool(tools::Registry& registry);

} // namespace Services::Weather
