#include "services/weather/Weather.h"
#include "services/weather/WeatherText.h"
#include "services/storage/SystemDatabase.h"
#include "common/thread_config.h"
#include "gemini_live/GeminiProtocol.h"
#include "media_player/TlsConfig.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace Services::Weather {
namespace {

const char* TAG = "Weather";
constexpr size_t MAX_BODY = 32 * 1024;
constexpr uint32_t WORKER_STACK = 8 * 1024;   // TLS handshake + ArduinoJson; PSRAM

struct Job {
    char call_id[64];
    std::string location;
    int days;
};

// The last place looked up; the home location is asked for most often.
std::mutex g_mutex;   // one lookup at a time; guards the cache
std::string g_cached_query;
std::string g_cached_place;
double g_cached_lat = 0, g_cached_lon = 0;

esp_err_t appendBody(esp_http_client_event_t* evt) {
    auto* body = static_cast<std::string*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0 && body->size() < MAX_BODY) {
        body->append(static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

bool httpGet(const std::string& url, std::string& body) {
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = appendBody;
    config.user_data = &body;
    config.timeout_ms = 8000;
    config.buffer_size = 2048;
    Tls::secure(config);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "GET failed: %s, HTTP %d", esp_err_to_name(err), status);
        return false;
    }
    return true;
}

// Fills place/lat/lon for `location`; false with `error` set if not found.
bool geocode(const std::string& location, std::string& place, double& lat, double& lon, const char*& error) {
    std::string city = ::Weather::cityPart(location);
    if (city.empty()) {
        error = "No location given and no home location set; ask the user for their city";
        return false;
    }
    if (city == g_cached_query) {
        place = g_cached_place;
        lat = g_cached_lat;
        lon = g_cached_lon;
        return true;
    }
    std::string body;
    if (!httpGet("https://geocoding-api.open-meteo.com/v1/search?count=1&format=json&name=" +
                     ::Weather::urlEncode(city),
                 body)) {
        error = "The weather service could not be reached";
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body) || doc["results"][0].isNull()) {
        error = "Place not found; ask the user for a nearby city";
        return false;
    }
    JsonObject r = doc["results"][0];
    place = r["name"] | city.c_str();
    const char* region = r["admin1"] | "";
    const char* country = r["country"] | "";
    if (region[0] && place != region) place += std::string(", ") + region;
    if (country[0]) place += std::string(", ") + country;
    lat = r["latitude"] | 0.0;
    lon = r["longitude"] | 0.0;
    g_cached_query = city;
    g_cached_place = place;
    g_cached_lat = lat;
    g_cached_lon = lon;
    return true;
}

// "2026-09-29T06:10" -> "06:10"
const char* clock(const char* iso) {
    const char* t = iso ? strchr(iso, 'T') : nullptr;
    return t ? t + 1 : (iso ? iso : "");
}

void lookup(const Job& job, JsonDocument& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string location = job.location;
    if (location.empty()) location = Services::loadSettings().weather_location;

    std::string place;
    double lat, lon;
    const char* error = nullptr;
    if (!geocode(location, place, lat, lon, error)) {
        out["status"] = "error";
        out["message"] = error;
        return;
    }
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,precipitation,weather_code,wind_speed_10m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
             "&timezone=auto&forecast_days=%d",
             lat, lon, job.days);
    std::string body;
    JsonDocument doc;
    if (!httpGet(url, body) || deserializeJson(doc, body)) {
        out["status"] = "error";
        out["message"] = "The weather service could not be reached";
        return;
    }
    body.clear();

    out["status"] = "success";
    out["place"] = place;
    out["units"] = "temperature C, wind km/h, precipitation mm";
    JsonObject cur = doc["current"];
    JsonObject now = out["current"].to<JsonObject>();
    now["time"] = clock(cur["time"]);
    now["conditions"] = ::Weather::describeCode(cur["weather_code"] | -1);
    now["temperature"] = cur["temperature_2m"];
    now["feels_like"] = cur["apparent_temperature"];
    now["humidity_pct"] = cur["relative_humidity_2m"];
    now["wind"] = cur["wind_speed_10m"];
    now["precipitation"] = cur["precipitation"];
    JsonObject d = doc["daily"];
    JsonArray days = out["daily"].to<JsonArray>();
    JsonArray dates = d["time"];
    for (size_t i = 0; i < dates.size(); ++i) {
        JsonObject day = days.add<JsonObject>();
        day["date"] = dates[i];
        day["conditions"] = ::Weather::describeCode(d["weather_code"][i] | -1);
        day["max"] = d["temperature_2m_max"][i];
        day["min"] = d["temperature_2m_min"][i];
        day["rain_chance_pct"] = d["precipitation_probability_max"][i];
        day["sunrise"] = clock(d["sunrise"][i]);
        day["sunset"] = clock(d["sunset"][i]);
    }
}

// Nothing with a destructor may be in scope at vTaskDeleteWithCaps.
void worker(void* arg) {
    {
        Job* job = static_cast<Job*>(arg);
        JsonDocument out;
        int64_t t0 = esp_timer_get_time();
        lookup(*job, out);
        std::string result;
        serializeJson(out, result);
        ESP_LOGI(TAG, "get_weather in %lld ms (stack left %u B): %.200s", (esp_timer_get_time() - t0) / 1000,
                 (unsigned)uxTaskGetStackHighWaterMark(nullptr), result.c_str());
        GeminiProtocol::getInstance().transmitToolResponse(job->call_id, result.c_str());
        delete job;
    }
    vTaskDeleteWithCaps(nullptr);
}

} // namespace

bool fetchAsync(const char* call_id, const std::string& location, int days) {
    Job* job = new Job{};
    strncpy(job->call_id, call_id, sizeof(job->call_id) - 1);
    job->location = location;
    job->days = std::clamp(days <= 0 ? 1 : days, 1, 7);
    if (xTaskCreatePinnedToCoreWithCaps(worker, "weather", WORKER_STACK, job, ThreadConfig::NORMAL, nullptr,
                                        ThreadConfig::CORE_NETWORK, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "Could not start the weather worker");
        delete job;
        return false;
    }
    return true;
}

} // namespace Services::Weather
