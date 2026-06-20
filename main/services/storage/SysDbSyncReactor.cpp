#include "SysDbSyncReactor.h"
#include "services/storage/StorageService.h"
#include "common/thread_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>

namespace Services {

SysDbSyncReactor& SysDbSyncReactor::getInstance() {
    static SysDbSyncReactor instance;
    return instance;
}

SysDbSyncReactor::SysDbSyncReactor()
    : ReactorTask({
          "sysdb_sync",
          ThreadConfig::StackSize::STACK_NORMAL,
          ThreadConfig::Priority::LOW,
          ThreadConfig::CORE_NETWORK,
          COMP::AUDIO | COMP::LED
      })
{}

bool SysDbSyncReactor::begin() {
    ESP_LOGI(TAG, "SysDbSyncReactor operational.");
    return true;
}

void SysDbSyncReactor::onStateChanged(ComponentMask changed, const SystemState& snap) {
    // We only react in run() by resetting the timeout upon getting notifications.
}

void SysDbSyncReactor::run() {
    ESP_LOGI(TAG, "SysDbSyncReactor task running.");

    TickType_t delay_ticks = portMAX_DELAY;
    bool pending_write = false;

    while (m_running) {
        uint32_t changed_bits = 0;
        // Block until notification, or timeout if we have a pending write
        BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &changed_bits, delay_ticks);
        if (!m_running) break;

        if (notified == pdTRUE && changed_bits > 0) {
            m_last_changed = changed_bits;
            
            // Check if mutated bits contain the fields we care about persisting
            bool has_audio_change = (m_last_changed & COMP::AUDIO) && 
                                    (m_last_changed & BIT_AUDIO::SPEAKER_VOLUME);
            bool has_led_change = (m_last_changed & COMP::LED) && 
                                  (m_last_changed & BIT_LED::COLOR);

            if (has_audio_change || has_led_change) {
                // We have a pending change. Reset or start the 3-second timer.
                pending_write = true;
                delay_ticks = pdMS_TO_TICKS(3000);
                ESP_LOGD(TAG, "Persistent state changed. Resetting debounce timer to 3s.");
            }
        } else {
            // Timeout expired! Write the state to SD card.
            if (pending_write) {
                writeStateToSD();
                pending_write = false;
                delay_ticks = portMAX_DELAY; // Sleep indefinitely until next change
            }
        }
    }
}

void SysDbSyncReactor::writeStateToSD() {
    if (!StorageService::getInstance().isMounted()) {
        return;
    }

    auto snap = EmbeddedSysDb::getInstance().snapshot();
    char buf[128];
    snprintf(buf, sizeof(buf),
             "speaker_volume=%d\nled_color=%d,%d,%d\n",
             snap.audio.speaker_volume,
             snap.led.color.r,
             snap.led.color.g,
             snap.led.color.b);

    if (StorageService::getInstance().writeFile("/sdcard/state_sync.txt", buf)) {
        ESP_LOGI(TAG, "Persistent state successfully synchronized to /sdcard/state_sync.txt");
    } else {
        ESP_LOGE(TAG, "Failed to write persistent state to SD card");
    }
}

} // namespace Services
