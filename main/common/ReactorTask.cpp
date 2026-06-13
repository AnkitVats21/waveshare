#include "common/ReactorTask.h"
#include "esp_log.h"

static const char* TAG = "ReactorTask";

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

ReactorTask::ReactorTask(const Config& cfg) : m_cfg(cfg) {
    m_running = false;
    m_task_handle = nullptr;
}

bool ReactorTask::start() {
    if (m_task_handle != nullptr) {
        return true; // Already started
    }

    m_running = true;

    BaseType_t result = xTaskCreatePinnedToCore(
        taskEntry,
        m_cfg.name,
        m_cfg.stack_size,
        this,
        m_cfg.priority,
        &m_task_handle,
        m_cfg.core_id
    );

    if (result != pdPASS || m_task_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to create task '%s'", m_cfg.name);
        m_running = false;
        return false;
    }

    // Register with SysDb so we receive notifications on state changes
    EmbeddedSysDb::getInstance().registerReactor(m_cfg.interest, m_task_handle);
    ESP_LOGI(TAG, "Started '%s' (core=%d, prio=%d)", m_cfg.name, (int)m_cfg.core_id, (int)m_cfg.priority);
    return true;
}

ReactorTask::~ReactorTask() {
    m_running = false;
    // Give one notification to unblock a waiting task so it can see m_running == false
    if (m_task_handle) {
        xTaskNotifyGive(m_task_handle);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// FreeRTOS task entry
// ─────────────────────────────────────────────────────────────────────────────

void ReactorTask::taskEntry(void* param) {
    static_cast<ReactorTask*>(param)->run();
    vTaskDelete(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Default run() loop
//
// Subclasses may override this entirely if they need a different structure
// (e.g., a timed poll mixed with state-change reactions).
// ─────────────────────────────────────────────────────────────────────────────

void ReactorTask::run() {
    while (m_running) {
        uint32_t changed_bits = 0;
        // Block until EmbeddedSysDb notifies us (via xTaskNotify with eSetBits)
        BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &changed_bits, portMAX_DELAY);
        if (!m_running) break;

        if (notified == pdTRUE && changed_bits > 0) {
            m_last_changed = changed_bits;
            // Take a snapshot of the current state and deliver to the subclass
            SystemState snap = EmbeddedSysDb::getInstance().snapshot();
            onStateChanged(m_last_changed, snap);
        }
    }
}
