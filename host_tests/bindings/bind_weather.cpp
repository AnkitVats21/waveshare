#include "bindings.h"
#include "services/weather/WeatherText.h"
#include <nanobind/stl/string.h>

void init_weather(nb::module_& m) {
    m.def("weather_describe_code", [](int code) { return std::string(Weather::describeCode(code)); });
    m.def("weather_url_encode", &Weather::urlEncode);
    m.def("weather_city_part", &Weather::cityPart);
}
