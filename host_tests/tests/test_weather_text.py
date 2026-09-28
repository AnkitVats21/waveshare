"""Portable helpers of the get_weather tool (main/services/weather/WeatherText)."""
import waveshare_host as wh


def test_codes():
    assert wh.weather_describe_code(0) == "clear sky"
    assert wh.weather_describe_code(61) == "light rain"
    assert wh.weather_describe_code(95) == "thunderstorm"
    assert wh.weather_describe_code(4) == "unknown"
    assert wh.weather_describe_code(-1) == "unknown"


def test_url_encode():
    assert wh.weather_url_encode("Pune") == "Pune"
    assert wh.weather_url_encode("New Delhi") == "New%20Delhi"
    assert wh.weather_url_encode("a&b=c") == "a%26b%3Dc"
    # UTF-8 bytes are encoded one by one
    assert wh.weather_url_encode("Zürich") == "Z%C3%BCrich"


def test_city_part():
    assert wh.weather_city_part("Paris, France") == "Paris"
    assert wh.weather_city_part("  Pune ") == "Pune"
    assert wh.weather_city_part("") == ""
    assert wh.weather_city_part(" , India") == ""
