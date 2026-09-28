#pragma once
// Portable helpers for the get_weather tool (host-tested).

#include <string>

namespace Weather {

// Words for a WMO weather code (Open-Meteo's weather_code), e.g. 61 ->
// "light rain"; "unknown" for codes outside the table.
const char* describeCode(int code);

// Percent-encodes everything but unreserved characters (RFC 3986), for a
// query parameter.
std::string urlEncode(const std::string& in);

// The city part of a location for Open-Meteo's name search, which matches
// place names only: "Paris, France" -> "Paris". Trims spaces.
std::string cityPart(const std::string& location);

} // namespace Weather
