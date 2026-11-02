#include "services/power/DeepSleep.h"
#include "app/led/LedService.h"
#include "app/audio/recording/AudioRecorder.h"
#include "app/media_player/MusicPlaybackService.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "hal/Board.h"
#include "hal/Board_defs.h"
#include "hal/led/LedStripManager.h"
#include "services/alarm/AlarmService.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctime>

namespace Services::DeepSleep {
namespace {

const char* TAG = "DeepSleep";
constexpr gpio_num_t KEY = GPIO_NUM_0;   // KEY_2, active low
// Before this the clock was never set: no alarm timer.
constexpr time_t MIN_VALID_EPOCH = 1700000000;

// What the alarm timer waits for (epoch s, 0 = none), kept over deep sleep
// so a tap that goes back to sleep keeps the same timer.
RTC_DATA_ATTR int64_t s_due = 0;

bool keyDown() { return gpio_get_level(KEY) == 0; }

void showFill(LedStripManager& leds, int lit) {
    for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
        if (i < lit) leds.setPixel(i, 60, 25, 0);
        else leds.setPixel(i, 0, 0, 0);
    }
    leds.refresh();
}

[[noreturn]] void sleepNow(int64_t due) {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext1_wakeup_io(1ULL << KEY, ESP_EXT1_WAKEUP_ANY_LOW);
    // The key's pull-up stays on while asleep.
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    rtc_gpio_pulldown_dis(KEY);
    rtc_gpio_pullup_en(KEY);

    s_due = due;
    const time_t now = time(nullptr);
    if (due > 0 && now > MIN_VALID_EPOCH) {
        int64_t s = due - ALARM_LEAD_S - now;
        if (s < 1) s = 1;
        esp_sleep_enable_timer_wakeup((uint64_t)s * 1000000ULL);
        ESP_LOGI(TAG, "Sleeping; KEY_2 wakes, or the timer in %lld s", (long long)s);
    } else {
        ESP_LOGI(TAG, "Sleeping; KEY_2 wakes");
    }
    esp_deep_sleep_start();
}

} // namespace

bool checkWake(LedStripManager& leds) {
    const uint32_t causes = esp_sleep_get_wakeup_causes();
    if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
        rtc_gpio_deinit(KEY);
        ESP_LOGI(TAG, "Woke for an alarm or reminder");
        return false;
    }
    if (!(causes & BIT(ESP_SLEEP_WAKEUP_EXT1))) return false;

    rtc_gpio_deinit(KEY);
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << KEY;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);

    const int64_t t0 = esp_timer_get_time();
    while (keyDown()) {
        const int64_t ms = (esp_timer_get_time() - t0) / 1000;
        if (ms >= WAKE_HOLD_MS) {
            showFill(leds, LED_STRIP_LED_COUNT);
            ESP_LOGI(TAG, "Woke by KEY_2");
            return true;
        }
        showFill(leds, 1 + (int)(ms * (LED_STRIP_LED_COUNT - 1) / WAKE_HOLD_MS));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    showFill(leds, 0);
    ESP_LOGI(TAG, "KEY_2 let go before %u ms; back to sleep", (unsigned)WAKE_HOLD_MS);
    sleepNow(s_due);
}

void enter() {
    ESP_LOGI(TAG, "Going to deep sleep");
    auto& recorder = AudioRecorder::getInstance();
    if (recorder.isRecording()) recorder.stopRecording(AudioRecorder::StopReason::MANUAL);
    MusicPlaybackService::getInstance().postCommand(MediaCmdType::PAUSE);
    // The chime ends; the recording and the music finish writing to the card.
    vTaskDelay(pdMS_TO_TICKS(800));

    int64_t due = AlarmService::getInstance().nextDue();
    const uint32_t snooze = EmbeddedSysDb::getInstance().snapshot().alarm.snooze_until;
    if (snooze && (due == 0 || snooze < due)) due = snooze;

    IoExpander& io = Board::getInstance().getIoExpanderInstance();
    io.setPowerRail(false);
    io.setLcdBacklight(false);
    LedService::setPowerHold(0);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_wifi_stop();
    // A held key would wake the chip at once.
    while (keyDown()) vTaskDelay(pdMS_TO_TICKS(20));
    sleepNow(due);
}

} // namespace Services::DeepSleep
