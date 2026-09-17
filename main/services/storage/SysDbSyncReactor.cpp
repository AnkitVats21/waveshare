#include "SysDbSyncReactor.h"
#include "services/storage/StorageService.h"
#include "common/thread_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "common/ParserUtils.h"
#include <cstdio>
#include <string>

namespace Services {

namespace {
struct StateParseCtx {
    int volume = 80;
    int r = 0, g = 0, b = 0;
    bool autoplay = true;
    bool cache_downloads = false;
    bool record_all_mic_channels = true;
};

static void onStatePair(const std::string& key, const std::string& val, void* ctx) {
    auto* p = static_cast<StateParseCtx*>(ctx);
    if (key == "speaker_volume") {
        p->volume = std::stoi(val);
    } else if (key == "led_color") {
        sscanf(val.c_str(), "%d,%d,%d", &p->r, &p->g, &p->b);
    } else if (key == "autoplay") {
        p->autoplay = (val == "1" || val == "true");
    } else if (key == "cache_downloads" || key == "caching") {
        p->cache_downloads = (val == "1" || val == "true");
    } else if (key == "record_all_mic_channels") {
        p->record_all_mic_channels = (val == "1" || val == "true");
    }
}
} // namespace

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
          COMP::AUDIO | COMP::LED | COMP::MEDIA
      })
{}

bool SysDbSyncReactor::begin() {
    ESP_LOGI(TAG, "SysDbSyncReactor operational.");
    return true;
}

bool SysDbSyncReactor::loadPersistentState() {
    if (!StorageService::getInstance().isMounted() ||
        !StorageService::getInstance().fileExists("/sdcard/state_sync.txt")) {
        return false;
    }

    std::string content = StorageService::getInstance().readFile("/sdcard/state_sync.txt");
    if (content.empty()) {
        return false;
    }

    StateParseCtx parseCtx;
    Utils::ParserUtils::parseKeyValueStream(content, onStatePair, &parseCtx);

    EmbeddedSysDb::getInstance().mutate([parseCtx](SystemState& s) {
        s.audio.speaker_volume = parseCtx.volume;
        s.led.color = { (uint8_t)parseCtx.r, (uint8_t)parseCtx.g, (uint8_t)parseCtx.b };
        s.led.mode = LedMode::SOLID;
        s.media.autoplay_enabled = parseCtx.autoplay;
        s.media.cache_downloads = parseCtx.cache_downloads;
        s.audio.record_all_mic_channels = parseCtx.record_all_mic_channels;
    });

    ESP_LOGI(TAG, "Persistent state restored from SD card: vol=%d, color=%d,%d,%d, autoplay=%d, cache_downloads=%d, record_all_mic_channels=%d",
             parseCtx.volume, parseCtx.r, parseCtx.g, parseCtx.b, parseCtx.autoplay, parseCtx.cache_downloads,
             parseCtx.record_all_mic_channels);
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
                                    (m_last_changed & (BIT_AUDIO::SPEAKER_VOLUME | BIT_AUDIO::RECORD_CHANNELS));
            bool has_media_change = (m_last_changed & COMP::MEDIA) && 
                                    (m_last_changed & (BIT_MEDIA::AUTOPLAY | BIT_MEDIA::CACHE));
            bool has_led_change = (m_last_changed & COMP::LED) && 
                                  (m_last_changed & BIT_LED::COLOR);

            if (has_audio_change || has_media_change || has_led_change) {
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
    char buf[192];
    snprintf(buf, sizeof(buf),
             "speaker_volume=%d\nled_color=%d,%d,%d\nautoplay=%d\ncache_downloads=%d\nrecord_all_mic_channels=%d\n",
             snap.audio.speaker_volume,
             snap.led.color.r,
             snap.led.color.g,
             snap.led.color.b,
             snap.media.autoplay_enabled ? 1 : 0,
             snap.media.cache_downloads ? 1 : 0,
             snap.audio.record_all_mic_channels ? 1 : 0);

    if (StorageService::getInstance().writeFile("/sdcard/state_sync.txt", buf)) {
        ESP_LOGI(TAG, "Persistent state successfully synchronized to /sdcard/state_sync.txt");
    } else {
        ESP_LOGE(TAG, "Failed to write persistent state to SD card");
    }
}

} // namespace Services
