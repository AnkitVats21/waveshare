#include "services/http/routes/Routes.h"
#include "services/http/FlashUpload.h"
#include "http_server/HttpUtil.h"
#include "http_server/WebBundle.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "core_sysdb/led_types.h"
#include "media_player/MusicPlaybackService.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>

namespace {

constexpr const char* TAG = "OtaRoutes";

esp_err_t sendUploadError(httpd_req_t* req, FlashUpload::Result r, esp_err_t err, const char* what) {
    using R = FlashUpload::Result;
    char msg[96];
    switch (r) {
        case R::BUSY:          return Http::sendError(req, 409, "Another flash upload is in progress");
        case R::WORKER_FAILED: return Http::sendError(req, 500, "Failed to start flash worker");
        case R::RECV_FAILED:   return Http::sendError(req, 500, "Upload interrupted");
        default:
            snprintf(msg, sizeof(msg), "%s failed: %s", what, esp_err_to_name(err));
            return Http::sendError(req, 500, msg);
    }
}

// ── Firmware ────────────────────────────────────────────────────────────────

class FirmwareSink : public FlashUpload::Sink {
public:
    explicit FirmwareSink(const esp_partition_t* part) : m_part(part) {}
    esp_err_t begin(size_t) override { return esp_ota_begin(m_part, OTA_WITH_SEQUENTIAL_WRITES, &m_handle); }
    esp_err_t write(const char* data, size_t len) override { return esp_ota_write(m_handle, data, len); }
    esp_err_t finish() override {
        esp_err_t err = esp_ota_end(m_handle);
        m_handle = 0;
        return (err == ESP_OK) ? esp_ota_set_boot_partition(m_part) : err;
    }
    void abort() override {
        if (m_handle) esp_ota_abort(m_handle);
        m_handle = 0;
    }

private:
    const esp_partition_t* m_part;
    esp_ota_handle_t m_handle = 0;
};

esp_err_t firmwareStatusHandler(httpd_req_t* req) {
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

esp_err_t firmwareUploadHandler(httpd_req_t* req) {
    if (req->content_len <= 0) {
        return Http::sendError(req, 400, "Empty payload for OTA update");
    }
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) {
        return Http::sendError(req, 500, "No OTA partition available for flashing");
    }

    ESP_LOGI(TAG, "Starting firmware OTA to '%s' (%u bytes)...", part->label, (unsigned)req->content_len);

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

    FirmwareSink sink(part);
    esp_err_t err = ESP_OK;
    FlashUpload::Result r = FlashUpload::receive(req, sink, &err);
    if (r != FlashUpload::Result::OK) {
        ESP_LOGE(TAG, "Firmware OTA failed (step %d): %s", (int)r, esp_err_to_name(err));
        sysdb.mutate([&](SystemState& s) { s.led = prev_led; });
        return sendUploadError(req, r, err, "Firmware OTA");
    }

    ESP_LOGI(TAG, "Firmware OTA complete, restarting in 2 s...");
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

// ── Frontend (web bundle A/B slots) ─────────────────────────────────────────

constexpr size_t SECTOR = 4096;

// Writes the bundle into the inactive slot, erasing sector by sector just
// ahead of the data, then verifies and activates it. The active slot is never
// touched, so a failed upload leaves the current frontend running.
class BundleSink : public FlashUpload::Sink {
public:
    explicit BundleSink(int slot) : m_slot(slot), m_part(Http::WebBundle::instance().partition(slot)) {}

    esp_err_t begin(size_t total) override {
        if (!m_part) return ESP_ERR_NOT_FOUND;
        if (total > m_part->size) return ESP_ERR_INVALID_SIZE;
        return ESP_OK;
    }
    esp_err_t write(const char* data, size_t len) override {
        size_t end = m_offset + len;
        if (end > m_erased) {
            size_t erase_end = (end + SECTOR - 1) / SECTOR * SECTOR;
            esp_err_t err = esp_partition_erase_range(m_part, m_erased, erase_end - m_erased);
            if (err != ESP_OK) return err;
            m_erased = erase_end;
        }
        esp_err_t err = esp_partition_write(m_part, m_offset, data, len);
        m_offset = end;
        return err;
    }
    esp_err_t finish() override { return Http::WebBundle::instance().activate(m_slot); }
    void abort() override {}

private:
    int m_slot;
    const esp_partition_t* m_part;
    size_t m_offset = 0;
    size_t m_erased = 0;
};

// Slot inspection maps the partitions, so it runs on the flash worker too.
struct FrontendStatus {
    Http::WebBundle::SlotInfo slots[Http::WebBundle::SLOT_COUNT];
};

esp_err_t collectStatus(void* p) {
    auto* st = static_cast<FrontendStatus*>(p);
    for (int i = 0; i < Http::WebBundle::SLOT_COUNT; ++i) {
        st->slots[i] = Http::WebBundle::instance().info(i);
    }
    return ESP_OK;
}

esp_err_t frontendStatusHandler(httpd_req_t* req) {
    auto& bundle = Http::WebBundle::instance();
    FrontendStatus st;
    if (FlashUpload::runInternal(collectStatus, &st) != ESP_OK) {
        return Http::sendError(req, 409, "Flash is busy, try again");
    }

    JsonDocument doc;
    doc["serving"] = bundle.available() ? "bundle" : "builtin";
    if (bundle.available()) doc["active_slot"] = bundle.activeSlot();
    else doc["active_slot"] = nullptr;
    if (bundle.uploadSlot() != Http::WebBundle::NO_SLOT) doc["upload_slot"] = bundle.uploadSlot();
    else doc["upload_slot"] = nullptr;

    JsonArray arr = doc["slots"].to<JsonArray>();
    for (int i = 0; i < Http::WebBundle::SLOT_COUNT; ++i) {
        const esp_partition_t* part = bundle.partition(i);
        if (!part) continue;
        JsonObject o = arr.add<JsonObject>();
        o["slot"] = i;
        o["label"] = part->label;
        o["capacity"] = part->size;
        o["valid"] = st.slots[i].valid;
        if (st.slots[i].valid) {
            o["version"] = st.slots[i].version;
            o["build_time"] = st.slots[i].build_time;
            o["size"] = st.slots[i].size;
            o["files"] = st.slots[i].files;
            o["sha256"] = st.slots[i].sha256;
        }
    }
    return Http::sendJson(req, 200, doc);
}

esp_err_t frontendUploadHandler(httpd_req_t* req) {
    auto& bundle = Http::WebBundle::instance();
    int slot = bundle.uploadSlot();
    if (slot == Http::WebBundle::NO_SLOT) {
        return Http::sendError(req, 501, "No frontend partitions (flash the new partition table over USB)");
    }
    if (req->content_len <= 0) {
        return Http::sendError(req, 400, "Empty payload for frontend update");
    }
    ESP_LOGI(TAG, "Frontend upload to %s (%u bytes)...", bundle.partition(slot)->label, (unsigned)req->content_len);

    BundleSink sink(slot);
    esp_err_t err = ESP_OK;
    FlashUpload::Result r = FlashUpload::receive(req, sink, &err);
    if (r != FlashUpload::Result::OK) {
        ESP_LOGE(TAG, "Frontend upload failed (step %d): %s", (int)r, esp_err_to_name(err));
        if (r == FlashUpload::Result::FINISH_FAILED) {
            return Http::sendError(req, 400, "Uploaded bundle is invalid (bad format or SHA-256 mismatch)");
        }
        return sendUploadError(req, r, err, "Frontend upload");
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Frontend updated";
    doc["active_slot"] = slot;
    return Http::sendJson(req, 200, doc);
}

esp_err_t activateOther(void* p) {
    auto& bundle = Http::WebBundle::instance();
    int target = bundle.available() ? 1 - bundle.activeSlot() : 0;
    *static_cast<int*>(p) = target;
    return bundle.activate(target);
}

esp_err_t frontendRollbackHandler(httpd_req_t* req) {
    if (Http::WebBundle::instance().uploadSlot() == Http::WebBundle::NO_SLOT) {
        return Http::sendError(req, 501, "No frontend partitions");
    }
    int target = -1;
    esp_err_t err = FlashUpload::runInternal(activateOther, &target);
    if (err == ESP_ERR_INVALID_STATE && target < 0) {
        return Http::sendError(req, 409, "Flash is busy, try again");
    }
    if (err != ESP_OK) {
        return Http::sendError(req, 409, "The other slot has no valid frontend");
    }
    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Switched frontend slot";
    doc["active_slot"] = target;
    return Http::sendJson(req, 200, doc);
}

} // namespace

void Routes::registerOta(Http::Server& server) {
    server.on("/api/ota/status", HTTP_GET, firmwareStatusHandler);
    server.on("/api/ota", HTTP_POST, firmwareUploadHandler);
    server.on("/api/ota/frontend", HTTP_GET, frontendStatusHandler);
    server.on("/api/ota/frontend", HTTP_POST, frontendUploadHandler);
    server.on("/api/ota/frontend/rollback", HTTP_POST, frontendRollbackHandler);
}
