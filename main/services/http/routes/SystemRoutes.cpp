#include "services/http/routes/Routes.h"
#include "services/http/routes/MusicStatus.h"
#include "services/http/SystemInfo.h"
#include "http_server/HttpUtil.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "core_sysdb/LogRouter.h"
#include "core_sysdb/led_types.h"
#include "app/audio/recording/AudioRecorder.h"

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <cstdlib>
#include <strings.h>

namespace {

uint32_t parseSeq(httpd_req_t* req, const char* param) {
    std::string s;
    return Http::queryParam(req, param, s) ? static_cast<uint32_t>(strtoul(s.c_str(), nullptr, 10)) : 0;
}

esp_err_t metricsHandler(httpd_req_t* req) {
    JsonDocument doc;

    doc["uptime_sec"] = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
    doc["num_tasks"] = uxTaskGetNumberOfTasks();
    int cpu0 = 0, cpu1 = 0;
    SystemInfo::getCpuUsage(cpu0, cpu1);
    doc["cpu0"] = cpu0;
    doc["cpu1"] = cpu1;
    doc["reset_reason"] = SystemInfo::resetReason();

    JsonObject heap = doc["heap"].to<JsonObject>();
    heap["internal_free"]     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    heap["internal_total"]    = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    heap["internal_min_free"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    heap["psram_free"]        = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    heap["psram_total"]       = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        wifi["connected"] = true;
        wifi["ssid"] = reinterpret_cast<const char*>(ap_info.ssid);
        wifi["rssi"] = ap_info.rssi;
        wifi["channel"] = ap_info.primary;
    } else {
        wifi["connected"] = false;
        wifi["rssi"] = 0;
    }
    std::string ip = SystemInfo::staIp();
    if (!ip.empty()) wifi["ip"] = ip;

    SystemState snap = EmbeddedSysDb::getInstance().snapshot();
    JsonObject audio = doc["audio"].to<JsonObject>();
    audio["speaker_volume"] = snap.audio.speaker_volume;
    audio["mic_gain_db"]    = snap.audio.mic_gain_db;
    audio["mic_enabled"]    = snap.audio.mic_enabled;
    audio["sample_rate"]    = snap.audio.sample_rate;
    audio["is_recording"]   = AudioRecorder::getInstance().isRecording();

    return Http::sendJson(req, 200, doc);
}

esp_err_t initHandler(httpd_req_t* req) {
    JsonDocument doc;
    doc["board"] = "ESP32-S3 (Waveshare)";
    const esp_app_desc_t* app_desc = esp_app_get_description();
    doc["version"]      = app_desc ? app_desc->version : "unknown";
    doc["compile_date"] = app_desc ? app_desc->date : "unknown";
    doc["compile_time"] = app_desc ? app_desc->time : "unknown";
    doc["reset_reason"] = SystemInfo::resetReason();
    doc["internal_total"] = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    doc["psram_total"]    = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* target  = esp_ota_get_next_update_partition(nullptr);
    doc["running_partition"] = running ? running->label : "unknown";
    doc["target_partition"]  = target ? target->label : "unknown";

    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        doc["wifi_ssid"]    = reinterpret_cast<const char*>(ap_info.ssid);
        doc["wifi_channel"] = ap_info.primary;
    }
    std::string ip = SystemInfo::staIp();
    if (!ip.empty()) doc["ip"] = ip;

    SystemState snap = EmbeddedSysDb::getInstance().snapshot();
    JsonObject state = doc["state"].to<JsonObject>();
    state["speaker_volume"] = snap.audio.speaker_volume;
    state["mic_gain_db"]    = snap.audio.mic_gain_db;
    state["mic_enabled"]    = snap.audio.mic_enabled;
    state["sample_rate"]    = snap.audio.sample_rate;
    state["is_recording"]   = AudioRecorder::getInstance().isRecording();
    state["led_mode"]       = static_cast<int>(snap.led.mode);
    state["led_r"]          = snap.led.color.r;
    state["led_g"]          = snap.led.color.g;
    state["led_b"]          = snap.led.color.b;

    return Http::sendJson(req, 200, doc);
}

// Polled by the built-in dashboard: fast-changing values every call, the
// control state only when it changed since the previous call.
esp_err_t deltaHandler(httpd_req_t* req) {
    JsonDocument doc;

    doc["up"] = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
    int cpu0 = 0, cpu1 = 0;
    SystemInfo::getCpuUsage(cpu0, cpu1);
    doc["c0"] = cpu0;
    doc["c1"] = cpu1;

    doc["sram"]     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    doc["min_sram"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    doc["psram"]    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    wifi_ap_record_t ap_info = {};
    doc["rssi"] = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) ? ap_info.rssi : 0;

    SystemState snap = EmbeddedSysDb::getInstance().snapshot();
    static int   s_last_vol = -1;
    static float s_last_gain = -1.0f;
    static bool  s_last_mic = false;
    static bool  s_last_rec = false;
    bool is_rec = AudioRecorder::getInstance().isRecording();

    if (snap.audio.speaker_volume != s_last_vol ||
        snap.audio.mic_gain_db != s_last_gain ||
        snap.audio.mic_enabled != s_last_mic ||
        is_rec != s_last_rec) {
        s_last_vol  = snap.audio.speaker_volume;
        s_last_gain = snap.audio.mic_gain_db;
        s_last_mic  = snap.audio.mic_enabled;
        s_last_rec  = is_rec;

        JsonObject state = doc["state"].to<JsonObject>();
        state["speaker_volume"] = s_last_vol;
        state["mic_gain_db"]    = s_last_gain;
        state["mic_enabled"]    = s_last_mic;
        state["is_recording"]   = s_last_rec;
    }

    std::vector<std::pair<uint32_t, std::string>> logs;
    uint32_t latest_seq = 0;
    LogRouter::getInstance().getLogsSince(parseSeq(req, "log_seq"), logs, latest_seq);
    doc["latest_seq"] = latest_seq;
    if (!logs.empty()) {
        JsonArray log_arr = doc["logs"].to<JsonArray>();
        for (const auto& l : logs) log_arr.add(l.second);
    }

    MusicStatus::fill(doc["music"].to<JsonObject>());
    return Http::sendJson(req, 200, doc);
}

esp_err_t logsHandler(httpd_req_t* req) {
    std::vector<std::pair<uint32_t, std::string>> logs;
    uint32_t latest_seq = 0;
    LogRouter::getInstance().getLogsSince(parseSeq(req, "since"), logs, latest_seq);

    JsonDocument doc;
    doc["latest_seq"] = latest_seq;
    JsonArray arr = doc["logs"].to<JsonArray>();
    for (const auto& l : logs) arr.add(l.second);
    return Http::sendJson(req, 200, doc);
}

esp_err_t ledSetHandler(httpd_req_t* req) {
    std::string body;
    if (req->content_len == 0 || req->content_len > 512 || !Http::readBody(req, body, 512)) {
        return Http::sendError(req, 400, "Missing JSON payload");
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        return Http::sendError(req, 400, "Invalid JSON payload");
    }

    const char* mode_str = doc["mode"] | "solid";
    int r = doc["r"] | 0;
    int g = doc["g"] | 0;
    int b = doc["b"] | 0;
    uint32_t speed = doc["speed_ms"] | 500;

    LedMode mode = LedMode::SOLID;
    if (strcasecmp(mode_str, "breath") == 0) mode = LedMode::BREATH;
    else if (strcasecmp(mode_str, "rainbow") == 0) mode = LedMode::RAINBOW;
    else if (strcasecmp(mode_str, "blink") == 0) mode = LedMode::BLINK;
    else if (strcasecmp(mode_str, "off") == 0) mode = LedMode::OFF;

    EmbeddedSysDb::getInstance().mutate([mode, r, g, b, speed](SystemState& s) {
        s.led.mode = mode;
        // Hardware WS2812 is wired GRB: struct fields {r, g, b} map to hardware {G, R, B}
        s.led.color = RgbColor{ static_cast<uint8_t>(g), static_cast<uint8_t>(r), static_cast<uint8_t>(b) };
        s.led.speed_ms = speed;
    });

    JsonDocument resp;
    resp["status"] = "ok";
    resp["mode"] = mode_str;
    return Http::sendJson(req, 200, resp);
}

esp_err_t rebootHandler(httpd_req_t* req) {
    static esp_timer_handle_t s_reboot_timer = nullptr;
    if (!s_reboot_timer) {
        esp_timer_create_args_t args = {};
        args.callback = [](void*) { esp_restart(); };
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "web_reboot";
        esp_timer_create(&args, &s_reboot_timer);
    }
    esp_timer_start_once(s_reboot_timer, 1500000ULL); // let the response go out first
    return Http::sendOk(req, "Rebooting device...");
}

} // namespace

void Routes::registerSystem(Http::Server& server) {
    server.on("/api/led/set", HTTP_POST, ledSetHandler);
    server.on("/api/system/metrics", HTTP_GET, metricsHandler);
    server.on("/api/system/init", HTTP_GET, initHandler);
    server.on("/api/system/delta", HTTP_GET, deltaHandler);
    server.on("/api/system/reboot", HTTP_POST, rebootHandler);
    server.on("/api/logs", HTTP_GET, logsHandler);
}
