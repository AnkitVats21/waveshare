#include "common/sysdb/EmbeddedSysDb.h"
#include "esp_log.h"

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────

EmbeddedSysDb& EmbeddedSysDb::getInstance() {
    static EmbeddedSysDb instance;
    return instance;
}

EmbeddedSysDb::EmbeddedSysDb() {
    // Counting semaphore: MAX_READERS permits — each reader takes one, writer takes all
    m_read_sem    = xSemaphoreCreateCounting(MAX_READERS, MAX_READERS);
    m_write_mutex = xSemaphoreCreateMutex();
    configASSERT(m_read_sem);
    configASSERT(m_write_mutex);
}

EmbeddedSysDb::~EmbeddedSysDb() {
    vSemaphoreDelete(m_read_sem);
    vSemaphoreDelete(m_write_mutex);
}

// ─────────────────────────────────────────────────────────────────────────────
// R/W lock implementation
// ─────────────────────────────────────────────────────────────────────────────

void EmbeddedSysDb::acquireRead() const {
    // Takes one of the MAX_READERS permits (blocks only when a writer holds all)
    xSemaphoreTake(m_read_sem, portMAX_DELAY);
}

void EmbeddedSysDb::releaseRead() const {
    xSemaphoreGive(m_read_sem);
}

void EmbeddedSysDb::acquireWrite() {
    // Serialise writers with the mutex, then drain all reader permits
    xSemaphoreTake(m_write_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_READERS; ++i) {
        xSemaphoreTake(m_read_sem, portMAX_DELAY);
    }
}

void EmbeddedSysDb::releaseWrite() {
    // Restore all reader permits, then release writer mutex
    for (int i = 0; i < MAX_READERS; ++i) {
        xSemaphoreGive(m_read_sem);
    }
    xSemaphoreGive(m_write_mutex);
}

// ─────────────────────────────────────────────────────────────────────────────
// Reader API
// ─────────────────────────────────────────────────────────────────────────────

SystemState EmbeddedSysDb::snapshot() const {
    acquireRead();
    SystemState copy = m_state;
    releaseRead();
    return copy;
}

bool EmbeddedSysDb::wifiConnected() const {
    acquireRead();
    bool v = m_state.system.wifi_connected;
    releaseRead();
    return v;
}

int EmbeddedSysDb::speakerVolume() const {
    acquireRead();
    int v = m_state.audio.speaker_volume;
    releaseRead();
    return v;
}

float EmbeddedSysDb::micGain() const {
    acquireRead();
    float v = m_state.audio.mic_gain_db;
    releaseRead();
    return v;
}

bool EmbeddedSysDb::assistantSpeaking() const {
    acquireRead();
    bool v = m_state.audio.assistant_speaking;
    releaseRead();
    return v;
}

bool EmbeddedSysDb::micEnabled() const {
    acquireRead();
    bool v = m_state.audio.mic_enabled;
    releaseRead();
    return v;
}

AssistantState EmbeddedSysDb::sessionState() const {
    acquireRead();
    AssistantState v = m_state.assistant.session_state;
    releaseRead();
    return v;
}

PipelineMode EmbeddedSysDb::pipelineMode() const {
    acquireRead();
    PipelineMode v = m_state.pipeline.mode;
    releaseRead();
    return v;
}

WsState EmbeddedSysDb::wsState() const {
    acquireRead();
    WsState v = m_state.assistant.ws_state;
    releaseRead();
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reactor registration
// ─────────────────────────────────────────────────────────────────────────────

void EmbeddedSysDb::registerReactor(ComponentMask interest, TaskHandle_t handle) {
    // Called at boot time only — no lock needed (tasks haven't started writing yet)
    if (m_reactor_count >= MAX_REACTORS) {
        ESP_LOGE(TAG, "MAX_REACTORS (%zu) exceeded — increase limit", MAX_REACTORS);
        return;
    }
    m_reactors[m_reactor_count++] = { interest, handle };
    ESP_LOGI(TAG, "Reactor registered (mask=0x%02lx, total=%zu)", (unsigned long)interest, m_reactor_count);
}

// ─────────────────────────────────────────────────────────────────────────────
// Reactor notification (called inside the write lock)
// ─────────────────────────────────────────────────────────────────────────────

void EmbeddedSysDb::notifyReactors_locked(ComponentMask changed) {
    for (size_t i = 0; i < m_reactor_count; ++i) {
        if (m_reactors[i].mask & changed) {
            // xTaskNotify is safe from task context.
            // Use eSetBits to combine changed masks so the reactor knows what changed.
            xTaskNotify(m_reactors[i].handle, changed, eSetBits);
        }
    }
}
