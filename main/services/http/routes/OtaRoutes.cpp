#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include "core_sysdb/led_types.h"
#include "media_player/MusicPlaybackService.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/semphr.h>

namespace {

constexpr const char* TAG = "OtaRoutes";

esp_err_t statusHandler(httpd_req_t* req) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    const esp_app_desc_t* app_desc = esp_app_get_description();

    JsonDocument doc;
    doc["running_partition"] = running ? running->label : "unknown";
    doc["target_partition"]  = target ? target->label : "unknown";
    doc["version"]           = app_desc ? app_desc->version : "unknown";
    doc["compile_date"]      = app_desc ? app_desc->date : "unknown";
    doc["compile_time"]      = app_desc ? app_desc->time : "unknown";
    return Http::sendJson(req, 200, doc);
}

// ── Flash writer on an internal-RAM stack ───────────────────────────────────
// The httpd task stack lives in PSRAM, but flash writes disable the cache, so
// esp_ota_* must run on a task whose stack is in internal RAM. The upload
// handler streams chunks and hands each one to this worker.

enum OtaCmd { OTA_CMD_NONE, OTA_CMD_BEGIN, OTA_CMD_WRITE, OTA_CMD_END, OTA_CMD_ABORT };

struct OtaWorkState {
    OtaCmd cmd = OTA_CMD_NONE;
    const esp_partition_t* partition = nullptr;
    const char* buf = nullptr;
    size_t length = 0;
    esp_err_t result = ESP_OK;
    esp_ota_handle_t ota_handle = 0;
    SemaphoreHandle_t req_sem = nullptr;
    SemaphoreHandle_t done_sem = nullptr;
    volatile bool running = false;
};

// Static TCB, stack and chunk buffer in internal SRAM (.bss), so the worker
// passes the cache-frozen stack check without a heap allocation.
StaticTask_t s_ota_tcb;
StackType_t  s_ota_stack[3072]; // 12 KB: esp_image validation + SHA-256
char         s_ota_chunk[4096]; // one flash sector per write

void otaWorkerThunk(void* arg) {
    auto* s = static_cast<OtaWorkState*>(arg);
    while (s->running) {
        if (xSemaphoreTake(s->req_sem, portMAX_DELAY) != pdTRUE) break;
        if (!s->running) break;

        switch (s->cmd) {
            case OTA_CMD_BEGIN:
                s->result = esp_ota_begin(s->partition, OTA_WITH_SEQUENTIAL_WRITES, &s->ota_handle);
                break;
            case OTA_CMD_WRITE:
                s->result = esp_ota_write(s->ota_handle, s->buf, s->length);
                break;
            case OTA_CMD_END:
                s->result = esp_ota_end(s->ota_handle);
                if (s->result == ESP_OK) {
                    s->result = esp_ota_set_boot_partition(s->partition);
                }
                break;
            case OTA_CMD_ABORT:
                if (s->ota_handle != 0) {
                    esp_ota_abort(s->ota_handle);
                    s->ota_handle = 0;
                }
                s->result = ESP_OK;
                break;
            default:
                s->result = ESP_OK;
                break;
        }
        xSemaphoreGive(s->done_sem);
    }
    vTaskDelete(nullptr);
}

// Runs one command on the worker and waits for its result.
esp_err_t runOtaCmd(OtaWorkState& state, OtaCmd cmd) {
    state.cmd = cmd;
    xSemaphoreGive(state.req_sem);
    xSemaphoreTake(state.done_sem, portMAX_DELAY);
    return state.result;
}

esp_err_t uploadHandler(httpd_req_t* req) {
    if (req->content_len <= 0) {
        return Http::sendError(req, 400, "Empty payload for OTA update");
    }

    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(nullptr);
    if (!update_partition) {
        return Http::sendError(req, 500, "No OTA partition available for flashing");
    }

    ESP_LOGI(TAG, "Starting OTA update to partition '%s' (size %u bytes)...",
             update_partition->label, (unsigned)req->content_len);

    // Stop music (frees CPU/network for the transfer) and blink green while
    // flashing. On failure the previous LED state is restored; on success the
    // device reboots anyway.
    MusicPlaybackService::getInstance().pause();
    auto& sysdb = EmbeddedSysDb::getInstance();
    const auto prev_led = sysdb.snapshot().led;
    sysdb.mutate([](SystemState& s) {
        s.led.mode = LedMode::BLINK;
        s.led.color = GREEN_LED;
        s.led.speed_ms = 250;
        s.led.repeat = 0;
    });
    auto restoreLed = [&]() { sysdb.mutate([&](SystemState& s) { s.led = prev_led; }); };

    OtaWorkState state;
    state.partition = update_partition;
    state.req_sem = xSemaphoreCreateBinary();
    state.done_sem = xSemaphoreCreateBinary();
    state.running = true;

    TaskHandle_t worker = xTaskCreateStatic(
        otaWorkerThunk, "ota_flasher",
        sizeof(s_ota_stack) / sizeof(s_ota_stack[0]),
        &state, ThreadConfig::Priority::LOW + 1,
        s_ota_stack, &s_ota_tcb);

    if (!worker) {
        vSemaphoreDelete(state.req_sem);
        vSemaphoreDelete(state.done_sem);
        restoreLed();
        return Http::sendError(req, 500, "Failed to create internal OTA worker");
    }

    auto stopWorker = [&]() {
        state.running = false;
        xSemaphoreGive(state.req_sem);
        vTaskDelay(pdMS_TO_TICKS(50));
        vSemaphoreDelete(state.req_sem);
        vSemaphoreDelete(state.done_sem);
    };

    if (esp_err_t err = runOtaCmd(state, OTA_CMD_BEGIN); err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        stopWorker();
        restoreLed();
        return Http::sendError(req, 500, "esp_ota_begin failed");
    }

    constexpr size_t CHUNK_SIZE = sizeof(s_ota_chunk);
    int remaining = req->content_len;
    bool success = true;

    while (remaining > 0) {
        int to_read = (remaining < static_cast<int>(CHUNK_SIZE)) ? remaining : static_cast<int>(CHUNK_SIZE);
        int received = httpd_req_recv(req, s_ota_chunk, to_read);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "OTA socket transfer interrupted");
            success = false;
            break;
        }

        state.buf = s_ota_chunk;
        state.length = received;
        if (esp_err_t err = runOtaCmd(state, OTA_CMD_WRITE); err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            success = false;
            break;
        }
        remaining -= received;
    }

    if (success) {
        if (esp_err_t err = runOtaCmd(state, OTA_CMD_END); err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end / set boot partition failed: %s", esp_err_to_name(err));
            success = false;
        }
    } else {
        runOtaCmd(state, OTA_CMD_ABORT);
    }
    stopWorker();

    if (!success) {
        restoreLed();
        return Http::sendError(req, 500, "OTA Flashing failed");
    }

    ESP_LOGI(TAG, "OTA Flashing complete! Scheduled restart in 2 seconds...");

    static esp_timer_handle_t s_reboot_timer = nullptr;
    if (!s_reboot_timer) {
        esp_timer_create_args_t args = {};
        args.callback = [](void*) { esp_restart(); };
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "ota_reboot";
        esp_timer_create(&args, &s_reboot_timer);
    }
    esp_timer_start_once(s_reboot_timer, 2000000ULL);

    return Http::sendOk(req, "OTA Flash successful! Device rebooting...");
}

} // namespace

void Routes::registerOta(Http::Server& server) {
    server.on("/api/ota/status", HTTP_GET, statusHandler);
    server.on("/api/ota", HTTP_POST, uploadHandler);
}
