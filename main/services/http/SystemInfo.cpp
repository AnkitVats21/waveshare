#include "services/http/SystemInfo.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>

namespace SystemInfo {

void getCpuUsage(int& cpu0_pct, int& cpu1_pct) {
#if (configGENERATE_RUN_TIME_STATS == 1)
    static uint32_t s_last_idle0 = 0;
    static uint32_t s_last_idle1 = 0;
    static int64_t  s_last_time_us = 0;
    static int      s_cached_cpu0 = 0;
    static int      s_cached_cpu1 = 0;

    int64_t now_us = esp_timer_get_time();
    uint32_t idle0 = ulTaskGetIdleRunTimeCounterForCore(0);
    uint32_t idle1 = ulTaskGetIdleRunTimeCounterForCore(1);

    if (s_last_time_us > 0) {
        int64_t dt_us = now_us - s_last_time_us;
        if (dt_us >= 250000) { // Refresh at least every 250ms
            float idle0_fraction = static_cast<float>(idle0 - s_last_idle0) / static_cast<float>(dt_us);
            float idle1_fraction = static_cast<float>(idle1 - s_last_idle1) / static_cast<float>(dt_us);

            if (idle0_fraction > 1.0f) idle0_fraction = 1.0f;
            if (idle0_fraction < 0.0f) idle0_fraction = 0.0f;
            if (idle1_fraction > 1.0f) idle1_fraction = 1.0f;
            if (idle1_fraction < 0.0f) idle1_fraction = 0.0f;

            s_cached_cpu0 = static_cast<int>((1.0f - idle0_fraction) * 100.0f);
            s_cached_cpu1 = static_cast<int>((1.0f - idle1_fraction) * 100.0f);

            s_last_idle0 = idle0;
            s_last_idle1 = idle1;
            s_last_time_us = now_us;
        }
    } else {
        s_last_idle0 = idle0;
        s_last_idle1 = idle1;
        s_last_time_us = now_us;
    }

    cpu0_pct = s_cached_cpu0;
    cpu1_pct = s_cached_cpu1;
#else
    cpu0_pct = 0;
    cpu1_pct = 0;
#endif
}

const char* resetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "Power-on Reset";
        case ESP_RST_SW:        return "Software Reset";
        case ESP_RST_PANIC:     return "Software Panic / Crash";
        case ESP_RST_INT_WDT:   return "Interrupt Watchdog";
        case ESP_RST_TASK_WDT:  return "Task Watchdog";
        case ESP_RST_BROWNOUT:  return "Brownout (Voltage Drop)";
        default:                return "Normal Boot";
    }
}

std::string staIp() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return {};
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) return {};
    char ip_str[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
    return ip_str;
}

} // namespace SystemInfo
