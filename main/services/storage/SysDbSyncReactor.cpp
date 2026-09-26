#include "SysDbSyncReactor.h"

#include "SystemDatabase.h"
#include "common/thread_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace Services {

namespace {
constexpr const char* STATE_KEY = "state";
}

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
    auto& db = systemDb();
    ndb::system::SavedState saved;
    if (!db.db().isOpen() || !db.state().get(STATE_KEY, saved)) return false;

    EmbeddedSysDb::getInstance().mutate([&saved](SystemState& s) { saved.toSysdb(s); });
    ESP_LOGI(TAG, "Saved state restored: vol=%d, mic_gain=%.1f dB, color=%d,%d,%d, led_mode=%d, autoplay=%d, "
             "cache_downloads=%d",
             (int)saved.speaker_volume, saved.mic_gain_db, saved.led_color.r, saved.led_color.g, saved.led_color.b,
             saved.led_mode, saved.autoplay, saved.cache_downloads);
    return true;
}

void SysDbSyncReactor::resumeSaving() {
    m_paused = false;
    if (getHandle()) xTaskNotify(getHandle(), COMP::AUDIO, eSetBits);  // save whatever changed meanwhile
}

void SysDbSyncReactor::onStateChanged(ComponentMask changed, const SystemState& snap) {
    // Handled in run(), which is woken by the same notification.
}

void SysDbSyncReactor::run() {
    ESP_LOGI(TAG, "SysDbSyncReactor task running.");

    // Save SAVE_DELAY_MS after the first change, rather than after the last:
    // media fields change continuously during playback and would otherwise
    // postpone the save forever. Saving an unchanged document writes nothing.
    bool pending = false;
    TickType_t deadline = 0;
    while (m_running) {
        TickType_t wait = portMAX_DELAY;
        if (pending) {
            TickType_t now = xTaskGetTickCount();
            wait = (int32_t)(deadline - now) > 0 ? deadline - now : 0;
        }
        uint32_t changed_bits = 0;
        BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &changed_bits, wait);
        if (!m_running) break;

        if (notified == pdTRUE && changed_bits != 0 && !pending) {
            pending = true;
            deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SAVE_DELAY_MS);
        }
        if (pending && (int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            save();
            pending = false;
        }
    }
}

void SysDbSyncReactor::save() {
    auto& db = systemDb();
    if (m_paused || !db.db().isOpen()) return;
    ndb::system::SavedState state;
    state.fromSysdb(EmbeddedSysDb::getInstance().snapshot());
    if (!db.state().put(STATE_KEY, state)) ESP_LOGE(TAG, "Failed to save state to system.ndb");
}

} // namespace Services
