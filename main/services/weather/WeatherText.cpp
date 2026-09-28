#include "services/weather/WeatherText.h"

namespace Weather {

const char* describeCode(int code) {
    switch (code) {
        case 0: return "clear sky";
        case 1: return "mainly clear";
        case 2: return "partly cloudy";
        case 3: return "overcast";
        case 45: return "fog";
        case 48: return "freezing fog";
        case 51: return "light drizzle";
        case 53: return "drizzle";
        case 55: return "heavy drizzle";
        case 56: return "light freezing drizzle";
        case 57: return "freezing drizzle";
        case 61: return "light rain";
        case 63: return "rain";
        case 65: return "heavy rain";
        case 66: return "light freezing rain";
        case 67: return "freezing rain";
        case 71: return "light snow";
        case 73: return "snow";
        case 75: return "heavy snow";
        case 77: return "snow grains";
        case 80: return "light showers";
        case 81: return "showers";
        case 82: return "violent showers";
        case 85: return "light snow showers";
        case 86: return "snow showers";
        case 95: return "thunderstorm";
        case 96: return "thunderstorm with light hail";
        case 99: return "thunderstorm with hail";
        default: return "unknown";
    }
}

std::string urlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string cityPart(const std::string& location) {
    std::string s = location.substr(0, location.find(','));
    size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(' ');
    return s.substr(a, b - a + 1);
}

} // namespace Weather
