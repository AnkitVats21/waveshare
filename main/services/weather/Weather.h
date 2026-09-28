#pragma once
// get_weather: current conditions and a daily forecast from Open-Meteo
// (keyless). The fetch (geocoding, then the forecast; ~1-2 s) runs on a
// short-lived worker task, which sends the tool response itself, so the
// Gemini protocol task keeps draining reply audio meanwhile.

#include <string>

namespace Services::Weather {

// Starts the lookup for `location` (empty: the weather_location setting)
// with `days` of forecast (clamped to 1-7). False if the worker could not
// be started; the caller then answers the call itself.
bool fetchAsync(const char* call_id, const std::string& location, int days);

} // namespace Services::Weather
