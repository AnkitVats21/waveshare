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
#include "media_player/TlsConfig.h"
#include "audio_core/WakeWordEngine.h"
#include "services/network/UsbNet.h"
#include "services/network/NetStats.h"
#include "services/network/WifiService.h"
#include "services/network/WifiSniff.h"

#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_flash.h>
#include <esp_partition.h>
#include <nvs.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_memory_utils.h>
#include <freertos/freertos_debug.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_private/wifi.h>
#include <lwip/stats.h>
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
    heap["internal_largest"]  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
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

void scheduleReboot() {
    static esp_timer_handle_t s_reboot_timer = nullptr;
    if (!s_reboot_timer) {
        esp_timer_create_args_t args = {};
        args.callback = [](void*) { esp_restart(); };
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "web_reboot";
        esp_timer_create(&args, &s_reboot_timer);
    }
    esp_timer_start_once(s_reboot_timer, 1500000ULL); // let the response go out first
}

esp_err_t rebootHandler(httpd_req_t* req) {
    scheduleReboot();
    return Http::sendOk(req, "Rebooting device...");
}

// What the USB port does: "serial" (Wi-Fi on) or "ethernet" (USB network,
// Wi-Fi off). See UsbNet.h.
esp_err_t usbModeGetHandler(httpd_req_t* req) {
    JsonDocument doc;
    doc["mode"] = UsbNet::modeName(UsbNet::savedMode());
    doc["fallback_sec"] = UsbNet::kFallbackSec;
    return Http::sendJson(req, 200, doc);
}

void putMac(JsonDocument& doc, const uint8_t mac[6]) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    doc["mac"] = buf;
}

// Station MAC override for network tests (WifiService::setRandomMac).
esp_err_t wifiMacGetHandler(httpd_req_t* req) {
    uint8_t mac[6];
    JsonDocument doc;
    doc["random"] = WifiService::randomMacEnabled(mac);
    putMac(doc, mac);
    return Http::sendJson(req, 200, doc);
}

// {"random": true | false}: a new random MAC, or back to the factory one;
// saves and reboots. The router then sees a new device (new IP via DHCP).
esp_err_t wifiMacSetHandler(httpd_req_t* req) {
    std::string body;
    if (req->content_len == 0 || req->content_len > 128 || !Http::readBody(req, body, 128)) {
        return Http::sendError(req, 400, "Missing JSON payload");
    }
    JsonDocument in;
    if (deserializeJson(in, body) || !in["random"].is<bool>()) {
        return Http::sendError(req, 400, "random must be true or false");
    }
    uint8_t mac[6];
    if (!WifiService::setRandomMac(in["random"].as<bool>(), mac)) {
        return Http::sendError(req, 500, "Could not save the MAC");
    }
    scheduleReboot();
    JsonDocument doc;
    doc["status"] = "ok";
    doc["random"] = in["random"].as<bool>();
    putMac(doc, mac);
    doc["rebooting"] = true;
    return Http::sendJson(req, 200, doc);
}

// {"level": 0-5} (none, error, warning, info, debug, verbose): the Wi-Fi
// driver's log level, at once and until reboot. Debug shows Block Ack
// (aggregation) setup and teardown per traffic class; lines go to /api/logs.
esp_err_t wifiLogSetHandler(httpd_req_t* req) {
    std::string body;
    if (req->content_len == 0 || req->content_len > 64 || !Http::readBody(req, body, 64)) {
        return Http::sendError(req, 400, "Missing JSON payload");
    }
    JsonDocument in;
    const int level = deserializeJson(in, body) ? -1 : (in["level"] | -1);
    if (level < 0 || level > 5) return Http::sendError(req, 400, "level must be 0-5");
    esp_log_level_set("wifi", static_cast<esp_log_level_t>(level));
    esp_err_t err = esp_wifi_internal_set_log_level(static_cast<wifi_log_level_t>(level));
    if (err == ESP_OK) err = esp_wifi_internal_set_log_mod(WIFI_LOG_MODULE_ALL, WIFI_LOG_SUBMODULE_ALL, true);
    if (err != ESP_OK) return Http::sendError(req, 500, esp_err_to_name(err));
    JsonDocument doc;
    doc["level"] = level;
    return Http::sendJson(req, 200, doc);
}

// {"seconds": 1-30}: starts a radio capture of the frames the AP sends
// the board (WifiSniff); GET reads the counters once it has ended.
esp_err_t wifiSniffSetHandler(httpd_req_t* req) {
    std::string body;
    if (req->content_len == 0 || req->content_len > 64 || !Http::readBody(req, body, 64)) {
        return Http::sendError(req, 400, "Missing JSON payload");
    }
    JsonDocument in;
    const int seconds = deserializeJson(in, body) ? 0 : (in["seconds"] | 0);
    if (seconds < 1 || seconds > 30) return Http::sendError(req, 400, "seconds must be 1-30");
    if (!WifiSniff::start(seconds)) return Http::sendError(req, 409, "Capture running or refused");
    JsonDocument doc;
    doc["started"] = true;
    doc["seconds"] = seconds;
    return Http::sendJson(req, 200, doc);
}

esp_err_t wifiSniffGetHandler(httpd_req_t* req) {
    const WifiSniff::Stats s = WifiSniff::stats();
    JsonDocument doc;
    doc["running"] = s.running;
    doc["seconds"] = s.seconds;
    doc["frames"] = s.frames;
    doc["non_qos"] = s.non_qos;
    doc["rx_errors"] = s.rx_errors;
    if (s.frames) doc["rssi_avg"] = s.rssi_sum / int32_t(s.frames);
    JsonObject tids = doc["tid"].to<JsonObject>();
    for (int i = 0; i < 8; ++i) {
        if (!s.tid[i].frames) continue;
        JsonObject t = tids[std::to_string(i)].to<JsonObject>();
        t["frames"] = s.tid[i].frames;
        t["bytes"] = s.tid[i].bytes;
        t["retries"] = s.tid[i].retries;
        t["aggregated"] = s.tid[i].aggregated;
    }
    JsonObject mcs = doc["ht_mcs"].to<JsonObject>();
    for (int i = 0; i < 16; ++i) if (s.ht_mcs[i]) mcs[std::to_string(i)] = s.ht_mcs[i];
    JsonObject legacy = doc["legacy_rate"].to<JsonObject>();
    for (int i = 0; i < 32; ++i) if (s.legacy[i]) legacy[std::to_string(i)] = s.legacy[i];
    JsonObject addba = doc["addba_req"].to<JsonObject>();
    JsonObject delba = doc["delba"].to<JsonObject>();
    for (int i = 0; i < 8; ++i) {
        if (s.addba_req[i]) addba[std::to_string(i)] = s.addba_req[i];
        if (s.delba[i]) delba[std::to_string(i)] = s.delba[i];
    }
    return Http::sendJson(req, 200, doc);
}

void putRxTuning(JsonDocument& doc) {
    WifiService::RxTuning t;
    doc["stored"] = WifiService::loadRxTuning(t);
    doc["static"] = t.static_rx;
    doc["dynamic"] = t.dynamic_rx;
    doc["ba_win"] = t.ba_win;
    doc["ampdu_rx"] = t.ampdu_rx == 0 ? "built-in" : (t.ampdu_rx == 2 ? "on" : "off");
    doc["no_11b"] = t.no_11b != 0;
}

// Wi-Fi receive tuning for network tests (WifiService::RxTuning); 0 means
// the built-in value.
esp_err_t wifiRxGetHandler(httpd_req_t* req) {
    JsonDocument doc;
    putRxTuning(doc);
    return Http::sendJson(req, 200, doc);
}

// {"static":N, "dynamic":N, "ba_win":N, "ampdu_rx":true|false, "no_11b":bool} or
// {"reset":true}: saves and reboots.
esp_err_t wifiRxSetHandler(httpd_req_t* req) {
    std::string body;
    if (req->content_len == 0 || req->content_len > 256 || !Http::readBody(req, body, 256)) {
        return Http::sendError(req, 400, "Missing JSON payload");
    }
    JsonDocument in;
    if (deserializeJson(in, body)) return Http::sendError(req, 400, "Invalid JSON payload");
    bool ok;
    if (in["reset"] | false) {
        ok = WifiService::saveRxTuning(nullptr);
    } else {
        WifiService::RxTuning t;
        const int st = in["static"] | 0, dy = in["dynamic"] | 0, ba = in["ba_win"] | 0;
        if (st < 0 || st > 25 || dy < 0 || dy > 128 || ba < 0 || ba > 32 || (ba && ba < 2) ||
            (st && st < 2)) {
            return Http::sendError(req, 400, "static 2-25, dynamic 0-128, ba_win 2-32");
        }
        t.static_rx = st;
        t.dynamic_rx = dy;
        t.ba_win = ba;
        if (in["ampdu_rx"].is<bool>()) t.ampdu_rx = in["ampdu_rx"].as<bool>() ? 2 : 1;
        t.no_11b = (in["no_11b"] | false) ? 1 : 0;
        ok = WifiService::saveRxTuning(&t);
    }
    if (!ok) return Http::sendError(req, 500, "Could not save");
    scheduleReboot();
    JsonDocument doc;
    putRxTuning(doc);
    doc["rebooting"] = true;
    return Http::sendJson(req, 200, doc);
}

// {"mode": "serial" | "ethernet"}: saves the mode and reboots into it.
esp_err_t usbModeSetHandler(httpd_req_t* req) {
    std::string body;
    if (req->content_len == 0 || req->content_len > 128 || !Http::readBody(req, body, 128)) {
        return Http::sendError(req, 400, "Missing JSON payload");
    }
    JsonDocument doc;
    UsbNet::Mode mode;
    if (deserializeJson(doc, body) || !UsbNet::parseMode(doc["mode"] | "", mode)) {
        return Http::sendError(req, 400, "mode must be \"serial\" or \"ethernet\"");
    }
    if (!UsbNet::saveMode(mode)) return Http::sendError(req, 500, "Could not save the mode");
    scheduleReboot();
    JsonDocument resp;
    resp["status"] = "ok";
    resp["mode"] = UsbNet::modeName(mode);
    resp["rebooting"] = true;
    return Http::sendJson(req, 200, resp);
}

// ── GET /api/system/tasks ───────────────────────────────────────────────
// Every task's stack: size, the least free since it started, and whether it
// is in internal RAM or PSRAM. For finding internal RAM to reclaim.
esp_err_t tasksHandler(httpd_req_t* req) {
    const UBaseType_t cap = uxTaskGetNumberOfTasks() + 8;
    auto* st = static_cast<TaskStatus_t*>(heap_caps_malloc(cap * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!st) return Http::sendError(req, 500, "Out of memory");
    const UBaseType_t n = uxTaskGetSystemState(st, cap, nullptr);

    JsonDocument doc;
    JsonObject heap = doc["heap"].to<JsonObject>();
    heap["internal_free"]     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    heap["internal_min_free"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    heap["internal_largest"]  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    heap["dma_free"]          = heap_caps_get_free_size(MALLOC_CAP_DMA);
    heap["psram_free"]        = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    heap["psram_largest"]     = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    uint32_t internalStacks = 0;
    JsonArray arr = doc["tasks"].to<JsonArray>();
    for (UBaseType_t i = 0; i < n; ++i) {
        const TaskStatus_t& t = st[i];
        // The stack grows down from pxEndOfStack (its high end) to pxStackBase.
        TaskSnapshot_t snap = {};
        const uint32_t size = vTaskGetSnapshot(t.xHandle, &snap) == pdTRUE
            ? reinterpret_cast<uintptr_t>(snap.pxEndOfStack) - reinterpret_cast<uintptr_t>(t.pxStackBase)
            : 0;
        const bool psram = esp_ptr_external_ram(t.pxStackBase);
        if (!psram) internalStacks += size;
        JsonObject o = arr.add<JsonObject>();
        o["name"] = t.pcTaskName;
        const BaseType_t core = xTaskGetCoreID(t.xHandle);
        o["core"] = core == tskNO_AFFINITY ? -1 : static_cast<int>(core);
        o["prio"] = t.uxCurrentPriority;
        o["stack"] = size;
        o["free_min"] = t.usStackHighWaterMark;  // bytes (StackType_t is a byte here)
        o["psram"] = psram;
    }
    heap_caps_free(st);
    doc["internal_stacks"] = internalStacks;
    return Http::sendJson(req, 200, doc);
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

// This task's run time so far (us).
uint32_t ownRunTimeUs() {
    TaskStatus_t st;
    vTaskGetInfo(nullptr, &st, pdFALSE, eRunning);
    return st.ulRunTimeCounter;
}

// lwIP's drop and error counters (CONFIG_LWIP_STATS), for differences over
// a network test. The counters are 16-bit; a test's differences fit.
struct LwipCounts {
    uint16_t link_recv, link_drop, link_memerr, ip_drop, tcp_recv, tcp_xmit, tcp_drop,
        tcp_memerr, tcp_err;
};

LwipCounts lwipCounts() {
#if LWIP_STATS
    return {(uint16_t)lwip_stats.link.recv, (uint16_t)lwip_stats.link.drop,
            (uint16_t)lwip_stats.link.memerr, (uint16_t)lwip_stats.ip.drop,
            (uint16_t)lwip_stats.tcp.recv, (uint16_t)lwip_stats.tcp.xmit,
            (uint16_t)lwip_stats.tcp.drop, (uint16_t)lwip_stats.tcp.memerr,
            (uint16_t)lwip_stats.tcp.err};
#else
    return {};
#endif
}

// Adds the receive-path counters since `rx0`/`lw0` to doc["rx"]: frames,
// frames refused by lwIP, pauses between frames, and lwIP's drops/errors.
void addRxDetail(JsonDocument& doc, const NetStats::RxDetail& rx0, const LwipCounts& lw0) {
    const NetStats::RxDetail rx = NetStats::rxDetail();
    const LwipCounts lw = lwipCounts();
    JsonObject o = doc["rx"].to<JsonObject>();
    o["frames"] = rx.frames - rx0.frames;
    o["refused"] = rx.refused - rx0.refused;
    o["gaps_100ms"] = rx.gaps_100ms;
    o["gaps_500ms"] = rx.gaps_500ms;
    o["gap_total_ms"] = rx.gap_total_ms;
    o["max_gap_ms"] = rx.max_gap_ms;
    JsonArray prec = o["precedence"].to<JsonArray>();
    for (uint32_t c : rx.precedence) prec.add(c);
#if LWIP_STATS
    JsonObject l = doc["lwip"].to<JsonObject>();
    l["link_recv"] = (uint16_t)(lw.link_recv - lw0.link_recv);
    l["link_drop"] = (uint16_t)(lw.link_drop - lw0.link_drop);
    l["link_memerr"] = (uint16_t)(lw.link_memerr - lw0.link_memerr);
    l["ip_drop"] = (uint16_t)(lw.ip_drop - lw0.ip_drop);
    l["tcp_recv"] = (uint16_t)(lw.tcp_recv - lw0.tcp_recv);
    l["tcp_xmit"] = (uint16_t)(lw.tcp_xmit - lw0.tcp_xmit);
    l["tcp_drop"] = (uint16_t)(lw.tcp_drop - lw0.tcp_drop);
    l["tcp_memerr"] = (uint16_t)(lw.tcp_memerr - lw0.tcp_memerr);
    l["tcp_err"] = (uint16_t)(lw.tcp_err - lw0.tcp_err);
#else
    (void)lw; (void)lw0;
#endif
}

// Network test: downloads up to max bytes (default 1 MB) from url, discards
// them and reports the rate, to measure the board's download speed one layer
// at a time (plain HTTP from the LAN, then HTTPS from the internet). Runs on
// the httpd task and blocks it for the download. ?quiet=1 pauses the
// wake-word pipeline for the test, as the upload test does.
esp_err_t netTestHandler(httpd_req_t* req) {
    std::string url, max_str;
    if (!Http::queryParam(req, "url", url) || url.empty()) {
        return Http::sendError(req, 400, "url required");
    }
    size_t max_bytes = Http::queryParam(req, "max", max_str) ? strtoul(max_str.c_str(), nullptr, 10) : 1024 * 1024;
    std::string quiet;
    const bool pause = Http::queryParam(req, "quiet", quiet) && quiet == "1" &&
                       !WakeWordEngine::getInstance().isStreamingActive();
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 4096;
    if (url.rfind("https://", 0) == 0) Tls::secure(cfg);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return Http::sendError(req, 500, "client init failed");
    if (pause) WakeWordEngine::getInstance().pauseProcessing();
    std::unique_ptr<char, decltype(&free)> buf(
        static_cast<char*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM)), &free);
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = buf ? esp_http_client_open(client, 0) : ESP_ERR_NO_MEM;
    int64_t t_first = 0;
    // CPU during the body: this task's run time and each core's idle time
    // (run-time counters are in us). This task does the TLS decryption.
    uint32_t task0 = 0, idle0 = 0, idle1 = 0;
    NetStats::RxDetail rx0 = {};
    LwipCounts lw0 = {};
    size_t total = 0;
    int status = 0;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        while (total < max_bytes) {
            int n = esp_http_client_read(client, buf.get(), 4096);
            if (n <= 0) break;
            if (total == 0) {
                t_first = esp_timer_get_time();
                task0 = ownRunTimeUs();
                idle0 = ulTaskGetIdleRunTimeCounterForCore(0);
                idle1 = ulTaskGetIdleRunTimeCounterForCore(1);
                NetStats::resetRxDetail();
                rx0 = NetStats::rxDetail();
                lw0 = lwipCounts();
            }
            total += n;
        }
    }
    const int64_t t_end = esp_timer_get_time();
    const uint32_t task_us = ownRunTimeUs() - task0;
    const uint32_t idle0_us = ulTaskGetIdleRunTimeCounterForCore(0) - idle0;
    const uint32_t idle1_us = ulTaskGetIdleRunTimeCounterForCore(1) - idle1;
    JsonDocument doc;
    if (t_first) addRxDetail(doc, rx0, lw0);  // before cleanup: the close adds frames
    esp_http_client_cleanup(client);
    if (pause) WakeWordEngine::getInstance().resumeProcessing();
    doc["status"] = status;
    doc["quiet"] = pause;
    doc["error"] = esp_err_to_name(err);
    doc["bytes"] = total;
    doc["connect_ms"] = (t_first ? t_first - t0 : t_end - t0) / 1000;
    const int64_t body_us = t_first ? t_end - t_first : 0;
    doc["body_ms"] = body_us / 1000;
    doc["kb_per_s"] = body_us > 0 ? (double)total / 1024.0 / ((double)body_us / 1e6) : 0;
    if (t_first) {
        doc["task_cpu_ms"] = task_us / 1000;
        doc["task_core"] = xPortGetCoreID();
        doc["idle0_ms"] = idle0_us / 1000;
        doc["idle1_ms"] = idle1_us / 1000;
    }
    return Http::sendJson(req, 200, doc);
}

// Upload half of the network test: the PC posts a body (curl --data-binary),
// the board reads and discards it and reports the rate. Measures the board's
// Wi-Fi + TCP receive path on the LAN, with no internet hop. ?quiet=1 pauses
// the wake-word pipeline (AFE on core 1) for the test, to see whether the
// audio work slows the network.
esp_err_t netTestUploadHandler(httpd_req_t* req) {
    std::string quiet;
    const bool pause = Http::queryParam(req, "quiet", quiet) && quiet == "1" &&
                       !WakeWordEngine::getInstance().isStreamingActive();
    if (pause) WakeWordEngine::getInstance().pauseProcessing();
    std::unique_ptr<char, decltype(&free)> buf(
        static_cast<char*>(heap_caps_malloc(8192, MALLOC_CAP_SPIRAM)), &free);
    size_t total = 0;
    int64_t t_first = 0;
    NetStats::RxDetail rx0 = {};
    LwipCounts lw0 = {};
    int timeouts = 0;  // a client that stops sending must not hold the httpd task
    while (buf && total < req->content_len) {
        int n = httpd_req_recv(req, buf.get(), 8192);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (n <= 0) break;
        timeouts = 0;
        if (total == 0) {
            t_first = esp_timer_get_time();
            NetStats::resetRxDetail();
            rx0 = NetStats::rxDetail();
            lw0 = lwipCounts();
        }
        total += n;
    }
    const int64_t t_end = esp_timer_get_time();
    if (pause) WakeWordEngine::getInstance().resumeProcessing();
    JsonDocument doc;
    if (t_first) addRxDetail(doc, rx0, lw0);
    doc["bytes"] = total;
    doc["quiet"] = pause;
    const int64_t body_us = t_first ? t_end - t_first : 0;
    doc["body_ms"] = body_us / 1000;
    doc["kb_per_s"] = body_us > 0 ? (double)total / 1024.0 / ((double)body_us / 1e6) : 0;
    return Http::sendJson(req, 200, doc);
}

} // namespace

void Routes::registerSystem(Http::Server& server) {
    server.on("/api/led/set", HTTP_POST, ledSetHandler);
    server.on("/api/system/metrics", HTTP_GET, metricsHandler);
    server.on("/api/system/init", HTTP_GET, initHandler);
    server.on("/api/system/delta", HTTP_GET, deltaHandler);
    server.on("/api/system/reboot", HTTP_POST, rebootHandler);
    server.on("/api/system/usb-mode", HTTP_GET, usbModeGetHandler);
    server.on("/api/system/usb-mode", HTTP_POST, usbModeSetHandler);
    server.on("/api/system/wifi-mac", HTTP_GET, wifiMacGetHandler);
    server.on("/api/system/wifi-mac", HTTP_POST, wifiMacSetHandler);
    server.on("/api/system/wifi-rx", HTTP_GET, wifiRxGetHandler);
    server.on("/api/system/wifi-rx", HTTP_POST, wifiRxSetHandler);
    server.on("/api/system/wifi-log", HTTP_POST, wifiLogSetHandler);
    server.on("/api/system/wifi-sniff", HTTP_GET, wifiSniffGetHandler);
    server.on("/api/system/wifi-sniff", HTTP_POST, wifiSniffSetHandler);
    server.on("/api/system/flash", HTTP_GET, flashHandler);
    server.on("/api/system/tasks", HTTP_GET, tasksHandler);
    server.on("/api/system/nettest", HTTP_GET, netTestHandler);
    server.on("/api/system/nettest", HTTP_POST, netTestUploadHandler);
    server.on("/api/logs", HTTP_GET, logsHandler);
}
