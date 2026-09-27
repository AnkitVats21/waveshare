#include "services/http/routes/Routes.h"
#include "services/http/routes/MusicStatus.h"
#include "services/http/SystemInfo.h"
#include "http_server/HttpUtil.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "core_sysdb/LogRouter.h"
#include "core_sysdb/led_types.h"
#include "app/audio/recording/AudioRecorder.h"
#include "services/http/FlashUpload.h"
#include "http_server/WebBundle.h"

#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_flash.h>
#include <esp_partition.h>
#include <nvs.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
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

// ── GET /api/system/flash ───────────────────────────────────────────────
// The partition table with how much of each partition is in use, where that
// can be told: app slots (the firmware image's length), the dashboard slots
// (the bundle), the wake-word models, NVS (entries). Flash is read on the
// FlashUpload worker: the httpd stack is in PSRAM.

constexpr int MAX_PARTS = 16;

struct PartUsage {
    const esp_partition_t* part = nullptr;
    int64_t used = -1;  // bytes; -1 = can't tell
    char version[32] = {};
    char detail[96] = {};
};

struct FlashReport {
    uint32_t chip_size = 0;
    int count = 0;
    PartUsage parts[MAX_PARTS];
};

// Length of the app image in an app slot, from its segment headers: 0 if the
// slot holds no image, -1 if the headers don't add up.
int64_t appImageLength(const esp_partition_t* p) {
    esp_image_header_t h;
    if (esp_partition_read(p, 0, &h, sizeof(h)) != ESP_OK) return -1;
    if (h.magic != ESP_IMAGE_HEADER_MAGIC) return 0;
    uint32_t pos = sizeof(h);
    for (int i = 0; i < h.segment_count; ++i) {
        esp_image_segment_header_t seg;
        if (esp_partition_read(p, pos, &seg, sizeof(seg)) != ESP_OK) return -1;
        pos += sizeof(seg) + seg.data_len;
        if (pos > p->size) return -1;
    }
    pos = (pos + 16) & ~15u;  // checksum byte, padded to 16 bytes
    if (h.hash_appended) pos += 32;
    return pos <= p->size ? pos : -1;
}

// esp-sr's model partition: a count, then per model a 32-byte name and a
// file count, then per file a 32-byte name, start and size. Used = the end
// of the last file.
int64_t modelsLength(const esp_partition_t* p, char* names, size_t names_len) {
    constexpr size_t HEAD = 2048, STR = 32;
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[HEAD]);
    if (!buf || esp_partition_read(p, 0, buf.get(), HEAD) != ESP_OK) return -1;
    auto i32 = [&](size_t off) { int32_t v; memcpy(&v, buf.get() + off, 4); return v; };
    size_t off = 0;
    const int32_t models = i32(off);
    off += 4;
    if (models <= 0 || models > 16) return 0;
    int64_t end = 0;
    names[0] = '\0';
    for (int m = 0; m < models; ++m) {
        if (off + STR + 4 > HEAD) return -1;
        char name[STR + 1] = {};
        memcpy(name, buf.get() + off, STR);
        if (names[0]) strlcat(names, ", ", names_len);
        strlcat(names, name, names_len);
        const int32_t files = i32(off + STR);
        off += STR + 4;
        if (files < 0 || off + size_t(files) * (STR + 8) > HEAD) return -1;
        for (int f = 0; f < files; ++f) {
            const int64_t start = i32(off + STR), size = i32(off + STR + 4);
            end = std::max(end, start + size);
            off += STR + 8;
        }
    }
    return end <= int64_t(p->size) ? end : -1;
}

esp_err_t collectFlash(void* arg) {
    auto* r = static_cast<FlashReport*>(arg);
    esp_flash_get_size(nullptr, &r->chip_size);
    auto& bundle = Http::WebBundle::instance();

    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    for (; it && r->count < MAX_PARTS; it = esp_partition_next(it)) {
        PartUsage& u = r->parts[r->count++];
        u.part = esp_partition_get(it);
        const esp_partition_t* p = u.part;
        if (p->type == ESP_PARTITION_TYPE_APP) {
            u.used = appImageLength(p);
            esp_app_desc_t desc;
            if (u.used > 0 && esp_ota_get_partition_description(p, &desc) == ESP_OK) {
                strlcpy(u.version, desc.version, sizeof(u.version));
            }
        } else if (p->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS) {
            nvs_stats_t st;
            if (nvs_get_stats(p->label, &st) == ESP_OK && st.total_entries) {
                u.used = int64_t(p->size) * st.used_entries / st.total_entries;
                snprintf(u.detail, sizeof(u.detail), "%u of %u entries", (unsigned)st.used_entries,
                         (unsigned)st.total_entries);
            }
        } else if (strcmp(p->label, "model") == 0) {
            u.used = modelsLength(p, u.detail, sizeof(u.detail));
        } else {
            for (int i = 0; i < Http::WebBundle::SLOT_COUNT; ++i) {
                if (bundle.partition(i) != p) continue;
                const auto info = bundle.info(i);
                u.used = info.valid ? info.size : 0;
                if (info.valid) strlcpy(u.version, info.version, sizeof(u.version));
            }
        }
    }
    esp_partition_iterator_release(it);
    return ESP_OK;
}

const char* subtypeName(const esp_partition_t* p) {
    if (p->type == ESP_PARTITION_TYPE_APP) {
        if (p->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) return "factory";
        if (p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) return "ota";
        return "app";
    }
    switch (p->subtype) {
        case ESP_PARTITION_SUBTYPE_DATA_OTA: return "otadata";
        case ESP_PARTITION_SUBTYPE_DATA_PHY: return "phy";
        case ESP_PARTITION_SUBTYPE_DATA_NVS: return "nvs";
        case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return "coredump";
        case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS: return "nvs_keys";
        case ESP_PARTITION_SUBTYPE_DATA_SPIFFS: return "spiffs";
        default: return "data";
    }
}

esp_err_t flashHandler(httpd_req_t* req) {
    auto report = std::make_unique<FlashReport>();
    if (FlashUpload::runInternal(collectFlash, report.get()) != ESP_OK) {
        return Http::sendError(req, 409, "Flash is busy, try again");
    }
    const esp_partition_t* running = esp_ota_get_running_partition();
    const auto& bundle = Http::WebBundle::instance();
    const esp_partition_t* liveWww = bundle.available() ? bundle.partition(bundle.activeSlot()) : nullptr;

    JsonDocument doc;
    doc["chip_size"] = report->chip_size;
    JsonArray arr = doc["partitions"].to<JsonArray>();
    for (int i = 0; i < report->count; ++i) {
        const PartUsage& u = report->parts[i];
        JsonObject o = arr.add<JsonObject>();
        o["label"] = u.part->label;
        o["type"] = u.part->type == ESP_PARTITION_TYPE_APP ? "app" : "data";
        o["subtype"] = subtypeName(u.part);
        o["offset"] = u.part->address;
        o["size"] = u.part->size;
        if (u.used >= 0) o["used"] = u.used;
        if (u.version[0]) o["version"] = u.version;
        if (u.detail[0]) o["detail"] = u.detail;
        if (u.part == running || u.part == liveWww) o["active"] = true;
    }
    return Http::sendJson(req, 200, doc);
}

} // namespace

void Routes::registerSystem(Http::Server& server) {
    server.on("/api/led/set", HTTP_POST, ledSetHandler);
    server.on("/api/system/metrics", HTTP_GET, metricsHandler);
    server.on("/api/system/init", HTTP_GET, initHandler);
    server.on("/api/system/delta", HTTP_GET, deltaHandler);
    server.on("/api/system/reboot", HTTP_POST, rebootHandler);
    server.on("/api/system/flash", HTTP_GET, flashHandler);
    server.on("/api/logs", HTTP_GET, logsHandler);
}
