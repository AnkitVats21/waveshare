#include "TimeSyncHelper.h"
#include "services/storage/StorageService.h"
#include "common/ParserUtils.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctime>
#include <cstring>

static const char* TAG = "TimeSyncHelper";

namespace Services {

static void onTimeSyncConfigPair(const std::string& key, const std::string& val, void* ctx) {
    auto* tz_str = static_cast<std::string*>(ctx);
    if (key == "system.timezone") {
        *tz_str = val;
    }
}

bool TimeSyncHelper::synchronizeTimeAndCleanup(uint32_t timeout_ms) {
    ESP_LOGI(TAG, "Initializing transient SNTP client...");

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    // Backup server
    esp_sntp_setservername(1, "time.google.com");
    
    esp_sntp_init();

    ESP_LOGI(TAG, "Waiting for NTP sync...");
    uint32_t elapsed_ms = 0;
    constexpr uint32_t check_interval_ms = 500;
    bool synced = false;

    while (elapsed_ms < timeout_ms) {
        sntp_sync_status_t status = esp_sntp_get_sync_status();
        if (status == SNTP_SYNC_STATUS_COMPLETED) {
            synced = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(check_interval_ms));
        elapsed_ms += check_interval_ms;
    }

    // Stop SNTP immediately to reclaim memory/sockets
    esp_sntp_stop();
    ESP_LOGI(TAG, "SNTP client stopped and cleaned up.");

    if (synced) {
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        char str[64];
        asctime_r(&timeinfo, str);
        ESP_LOGI(TAG, "NTP synchronization successful! System time: %s", str);

        // Load timezone from settings.txt
        std::string tz = "UTC"; // default
        if (StorageService::getInstance().isMounted()) {
            std::string config = StorageService::getInstance().readFile("/sdcard/settings.txt");
            if (!config.empty()) {
                Utils::ParserUtils::parseKeyValueStream(config, onTimeSyncConfigPair, &tz);
                if (tz != "UTC") {
                    ESP_LOGI(TAG, "Loaded timezone configuration from SD card: %s", tz.c_str());
                }
            }
        }

        // Apply timezone
        setenv("TZ", tz.c_str(), 1);
        tzset();
        
        // Log timezone-adjusted local time
        time(&now);
        localtime_r(&now, &timeinfo);
        asctime_r(&timeinfo, str);
        ESP_LOGI(TAG, "Timezone set. Local time is now: %s", str);
        return true;
    } else {
        ESP_LOGW(TAG, "NTP synchronization timed out. Continuing with default RTC time.");
        return false;
    }
}

} // namespace Services
