#include "TimeSyncHelper.h"
#include "services/storage/SystemDatabase.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <algorithm>
#include <ctime>
#include <cstring>
#include <sys/time.h>

static const char* TAG = "TimeSync";

namespace Services {

namespace {

// Tried in turn, one per attempt (CONFIG_LWIP_SNTP_MAX_SERVERS is 1).
const char* const SERVERS[] = {"pool.ntp.org", "time.google.com", "time.cloudflare.com"};
constexpr size_t SERVER_COUNT = sizeof(SERVERS) / sizeof(SERVERS[0]);

constexpr uint32_t ATTEMPT_TIMEOUT_MS = 15000;
// Backoff after consecutive failures; the last value repeats.
constexpr uint32_t RETRY_S[] = {30, 60, 120, 300, 600, 900};
constexpr uint32_t RESYNC_S = 24 * 3600;

// 2021-01-01 .. 2100-01-01
constexpr int64_t MIN_EPOCH = 1609459200;
constexpr int64_t MAX_EPOCH = 4102444800;

void logLocalTime(const char* what) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    ESP_LOGI(TAG, "%s; local time %s", what, buf);
}

} // namespace

TimeSyncHelper& TimeSyncHelper::instance() {
    static TimeSyncHelper inst;
    return inst;
}

void TimeSyncHelper::applyTimezone(const std::string& tz) {
    setenv("TZ", tz.empty() ? "UTC" : tz.c_str(), 1);
    tzset();
}

bool TimeSyncHelper::clockValid() {
    return time(nullptr) >= MIN_EPOCH;
}

void TimeSyncHelper::begin() {
    // The timezone does not depend on a sync: a clock set by hand needs it too.
    applyTimezone(loadSettings().timezone);
    if (m_task) return;
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(taskEntry, "time_sync", 4096, this, 3, &m_task, 0,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        m_task = nullptr;
        ESP_LOGE(TAG, "Cannot start the time sync task");
    }
}

void TimeSyncHelper::onWifiConnected() {
    m_wifi = true;
    if (!clockValid() || m_status.source != Source::Ntp) requestSync();
}

void TimeSyncHelper::requestSync() {
    m_sync_now = true;
    if (m_task) xTaskNotifyGive(m_task);
}

bool TimeSyncHelper::setTime(int64_t epoch_s) {
    if (epoch_s < MIN_EPOCH || epoch_s > MAX_EPOCH) return false;
    struct timeval tv = {static_cast<time_t>(epoch_s), 0};
    settimeofday(&tv, nullptr);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.valid = true;
        m_status.source = Source::Manual;
    }
    logLocalTime("Clock set by hand");
    return true;
}

TimeSyncHelper::Status TimeSyncHelper::status() {
    std::lock_guard<std::mutex> lock(m_mutex);
    Status st = m_status;
    st.valid = clockValid();
    const int64_t now = esp_timer_get_time();
    st.next_attempt_s = m_next_attempt_us > now ? (uint32_t)((m_next_attempt_us - now) / 1000000) : 0;
    return st;
}

void TimeSyncHelper::taskEntry(void* arg) {
    static_cast<TimeSyncHelper*>(arg)->run();
}

bool TimeSyncHelper::attempt(const char* server) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.syncing = true;
        m_status.server = server;
    }
    ESP_LOGI(TAG, "NTP sync with %s...", server);
    const int64_t t0 = esp_timer_get_time();

    // Transient client: started per attempt and stopped after, to free its socket.
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, server);
    sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    esp_sntp_init();
    bool synced = false;
    for (uint32_t waited = 0; waited < ATTEMPT_TIMEOUT_MS; waited += 250) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            synced = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    esp_sntp_stop();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_status.syncing = false;
    if (synced) {
        m_status.valid = true;
        m_status.source = Source::Ntp;
        m_status.last_sync = time(nullptr);
        m_status.failures = 0;
    } else {
        ++m_status.failures;
    }
    ESP_LOGI(TAG, "NTP %s with %s after %lld ms", synced ? "synced" : "failed", server,
             (long long)((esp_timer_get_time() - t0) / 1000));
    return synced;
}

void TimeSyncHelper::run() {
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (m_next_attempt_us > 0) {
            const int64_t left_us = m_next_attempt_us - esp_timer_get_time();
            wait = left_us > 0 ? pdMS_TO_TICKS(left_us / 1000 + 1) : 0;
        }
        ulTaskNotifyTake(pdTRUE, wait);

        const bool due = m_sync_now || (m_next_attempt_us > 0 && esp_timer_get_time() >= m_next_attempt_us);
        if (!due) continue;
        m_sync_now = false;
        if (!m_wifi) {
            m_next_attempt_us = 0;   // onWifiConnected() starts over
            continue;
        }

        const char* server = SERVERS[m_server_index % SERVER_COUNT];
        uint32_t next_s;
        if (attempt(server)) {
            logLocalTime("Clock synced");
            next_s = RESYNC_S;
        } else {
            // Next attempt goes to the next server.
            ++m_server_index;
            uint32_t failures;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                failures = m_status.failures;
            }
            const size_t step = std::min<size_t>(failures, sizeof(RETRY_S) / sizeof(RETRY_S[0])) - 1;
            next_s = RETRY_S[step];
            ESP_LOGW(TAG, "Clock %s; retrying in %u s", clockValid() ? "kept as is" : "not set",
                     (unsigned)next_s);
        }
        m_next_attempt_us = esp_timer_get_time() + (int64_t)next_s * 1000000;
    }
}

} // namespace Services
