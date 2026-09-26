#include "nexus_db/StressTest.h"

#include "sdkconfig.h"

#if CONFIG_NEXUS_DB_STRESS_TEST

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nexus_db/Database.h"
#include "nexus_db/Fields.h"
#include "sd_storage/Fs.h"

namespace nexus_db {
namespace {

const char* const TAG = "ndb_stress";
constexpr const char* RUN_FILE = "/sdcard/db/stress.run";
constexpr const char* LOG_FILE = "/sdcard/db/stress.log";
constexpr const char* DB_PATH = "/sdcard/db/stress.ndb";
constexpr uint32_t MAGIC = 0x4E445354;  // "NDST"
constexpr uint8_t DOCS = 1, RAW = 2;
constexpr uint32_t DOC_SLOTS = 16, RAW_SLOTS = 8;

// Survives esp_restart (not a power cycle).
struct Persist {
    uint32_t magic;
    uint32_t acked;  // last counter whose meta merge returned true
    uint32_t pass;
    uint32_t fail;
};
RTC_NOINIT_ATTR Persist s_rtc;

const CollectionDef COLLECTIONS[] = {{DOCS, "docs", true}, {RAW, "raw", false}};

Options options() {
    Options o{};
    o.name = "stress";
    o.path = DB_PATH;
    o.schema_hash = 0;
    o.flush = Flush::EveryCommit;
    o.collections = COLLECTIONS;
    o.collection_count = 2;
    o.compact_min_bytes = 8 * 1024;  // frequent cleanups, so resets hit them too
    return o;
}

// Value: counter (tag 1), payload (tag 2), CRC of counter+payload (tag 3).
std::string makeValue(uint32_t n) {
    std::string payload(esp_random() % 200, '\0');
    for (char& c : payload) c = char(esp_random());
    uint32_t crc = crc32(&n, 4);
    crc = crc32(payload.data(), payload.size(), crc);
    std::string v;
    Writer w(v);
    w.u32(1, n);
    w.bytes(2, payload);
    w.u32(3, crc);
    return v;
}

// Counter in the value, or -1 if the value is inconsistent.
int64_t checkValue(std::string_view v) {
    FieldReader r(reinterpret_cast<const uint8_t*>(v.data()), v.size());
    uint16_t tag, len;
    const uint8_t* p;
    uint32_t n = 0, crc = 0;
    std::string payload;
    int seen = 0;
    while (r.next(tag, p, len)) {
        if (tag == 1 && FieldReader::read(p, len, n)) seen |= 1;
        if (tag == 2 && FieldReader::read(p, len, payload)) seen |= 2;
        if (tag == 3 && FieldReader::read(p, len, crc)) seen |= 4;
    }
    if (seen != 7) return -1;
    uint32_t want = crc32(&n, 4);
    want = crc32(payload.data(), payload.size(), want);
    return want == crc ? int64_t(n) : -1;
}

void logLine(const char* line) {
    ESP_LOGI(TAG, "%s", line);
    std::string l(line);
    l += '\n';
    sd_storage::Fs::append(LOG_FILE, l.data(), l.size());
}

// Slot `slot` of `slots` must hold the latest acknowledged counter with that
// residue, or the in-flight one (acked + 1).
bool slotOk(int64_t found, uint32_t slot, uint32_t slots, uint32_t acked) {
    int64_t latest = -1;
    if (acked >= 1) {
        int64_t m = int64_t(acked) - int64_t((acked + slots - slot) % slots);
        if (m >= 1) latest = m;
    }
    bool inflight = (acked + 1) % slots == slot && found == int64_t(acked) + 1;
    if (latest < 0) return found < 0 || inflight;  // never written (or only in flight)
    return found == latest || inflight;
}

void IRAM_ATTR restartNow(void*) { esp_restart(); }

// One round. Runs in its own task: the main task's stack is only 3.5 KB.
void runRound() {
    std::string run = sd_storage::Fs::readText(RUN_FILE, 64);
    if (run.empty()) return;
    int rounds = atoi(run.c_str());
    bool fresh = s_rtc.magic != MAGIC || esp_reset_reason() != ESP_RST_SW;
    if (fresh) {
        // First round (or a real power cycle): start over.
        s_rtc = {MAGIC, 0, 0, 0};
        sd_storage::Fs::remove(DB_PATH);
        sd_storage::Fs::remove((std::string(DB_PATH) + ".tmp").c_str());
        logLine("--- stress test start ---");
    }

    Database db(options(), sdIo());
    int64_t t0 = esp_timer_get_time();
    bool opened = db.open();
    int open_ms = int((esp_timer_get_time() - t0) / 1000);
    Stats st = db.stats();

    // Check everything the last round acknowledged.
    uint32_t acked = s_rtc.acked;
    int64_t meta = -1;
    std::string v;
    if (opened && db.get(DOCS, "meta", v)) {
        uint32_t n = 0;
        uint16_t tag, len;
        const uint8_t* p;
        FieldReader r(reinterpret_cast<const uint8_t*>(v.data()), v.size());
        while (r.next(tag, p, len)) {
            if (tag == 1) FieldReader::read(p, len, n);
        }
        meta = n;
    }
    bool ok = opened && (fresh ? meta < 0 : (meta == acked || meta == int64_t(acked) + 1 || (acked == 0 && meta < 0)));
    int bad_slots = 0;
    for (uint32_t s = 0; opened && s < DOC_SLOTS; ++s) {
        int64_t found = db.get(DOCS, "k" + std::to_string(s), v) ? checkValue(v) : -1;
        if (!slotOk(found, s, DOC_SLOTS, acked)) bad_slots++;
    }
    for (uint32_t s = 0; opened && s < RAW_SLOTS; ++s) {
        int64_t found = db.get(RAW, "r" + std::to_string(s), v) ? checkValue(v) : -1;
        if (!slotOk(found, s, RAW_SLOTS, acked)) bad_slots++;
    }
    ok = ok && bad_slots == 0;
    if (!fresh) ok ? s_rtc.pass++ : s_rtc.fail++;

    char line[256];
    snprintf(line, sizeof(line),
             "round %d: %s acked=%" PRIu32 " meta=%" PRId64 " bad_slots=%d open=%dms docs=%" PRIu32
             " bytes=%" PRIu32 " truncated=%" PRIu32 " skipped=%" PRIu32 " gen=%" PRIu32,
             rounds, fresh ? "START" : (ok ? "PASS" : "FAIL"), acked, meta, bad_slots, open_ms, st.documents,
             st.file_bytes, st.truncated_bytes, st.skipped_bytes, st.generation);
    logLine(line);

    if (!opened || rounds <= 0) {
        snprintf(line, sizeof(line), "NDB_STRESS DONE pass=%" PRIu32 " fail=%" PRIu32, s_rtc.pass, s_rtc.fail);
        logLine(line);
        sd_storage::Fs::remove(RUN_FILE);
        s_rtc.magic = 0;
        return;
    }
    char left[16];
    snprintf(left, sizeof(left), "%d", rounds - 1);
    sd_storage::Fs::writeAtomic(RUN_FILE, left);

    // Continue from what the file holds, then reset at a random moment.
    uint32_t n = meta > 0 ? uint32_t(meta) : 0;
    s_rtc.acked = n;
    esp_timer_handle_t timer;
    esp_timer_create_args_t args = {};
    args.callback = restartNow;
    args.name = "ndb_reset";
    esp_timer_create(&args, &timer);
    esp_timer_start_once(timer, 200000 + esp_random() % 2800000);  // 0.2..3 s
    while (true) {
        ++n;
        db.put(DOCS, "k" + std::to_string(n % DOC_SLOTS), makeValue(n));
        db.put(RAW, "r" + std::to_string(n % RAW_SLOTS), makeValue(n));
        std::string m;
        Writer(m).u32(1, n);
        if (db.merge(DOCS, "meta", m)) s_rtc.acked = n;
    }
}

void roundTask(void* arg) {
    runRound();
    xTaskNotifyGive(static_cast<TaskHandle_t>(arg));
    vTaskDelete(nullptr);
}

}  // namespace

void runStressTestIfRequested() {
    if (!sd_storage::Fs::isFile(RUN_FILE)) return;
    if (xTaskCreate(roundTask, "ndb_stress", 12 * 1024, xTaskGetCurrentTaskHandle(), 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "could not start the stress task");
        return;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // returns when no rounds remain
}

}  // namespace nexus_db

#endif  // CONFIG_NEXUS_DB_STRESS_TEST
