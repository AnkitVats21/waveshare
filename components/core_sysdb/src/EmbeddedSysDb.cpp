#include "core_sysdb/EmbeddedSysDb.h"
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
    updateHotAudioFlags_locked();
}

void EmbeddedSysDb::updateHotAudioFlags_locked() {
    uint32_t hot = 0;
    if (m_state.audio.assistant_speaking)     hot |= HotAudioBit::ASST_SPEAKING;
    if (m_state.audio.turn_complete_pending)  hot |= HotAudioBit::TURN_COMPLETE_PEND;
    if (m_state.bluetooth.connected)          hot |= HotAudioBit::BT_CONNECTED;
    m_hot_audio_flags.store(hot, std::memory_order_release);
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

NetworkState EmbeddedSysDb::networkState() const {
    acquireRead();
    NetworkState v = m_state.system.network_state;
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

bool EmbeddedSysDb::turnCompletePending() const {
    acquireRead();
    bool v = m_state.audio.turn_complete_pending;
    releaseRead();
    return v;
}

bool EmbeddedSysDb::alarmPlaying() const {
    acquireRead();
    bool v = m_state.alarm.playing;
    releaseRead();
    return v;
}

bool EmbeddedSysDb::alarmStopRequested() const {
    acquireRead();
    bool v = m_state.alarm.stop_requested;
    releaseRead();
    return v;
}

MediaPlaybackState EmbeddedSysDb::mediaState() const {
    acquireRead();
    MediaPlaybackState v = m_state.media.state;
    releaseRead();
    return v;
}

bool EmbeddedSysDb::isMediaDucked() const {
    acquireRead();
    bool v = m_state.media.is_ducked;
    releaseRead();
    return v;
}

bool EmbeddedSysDb::autoplayEnabled() const {
    acquireRead();
    bool v = m_state.media.autoplay_enabled;
    releaseRead();
    return v;
}

bool EmbeddedSysDb::cacheDownloads() const {
    acquireRead();
    bool v = m_state.media.cache_downloads;
    releaseRead();
    return v;
}

bool EmbeddedSysDb::bluetoothConnected() const {
    return hotBluetoothConnected();
}

MediaOutputTarget EmbeddedSysDb::mediaOutputTarget() const {
    acquireRead();
    MediaOutputTarget v = m_state.media.output_target;
    releaseRead();
    return v;
}

MediaPendingCommand EmbeddedSysDb::mediaPendingCommand() const {
    acquireRead();
    MediaPendingCommand v = m_state.media.pending_command;
    releaseRead();
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reactor registration
// ─────────────────────────────────────────────────────────────────────────────

void EmbeddedSysDb::registerReactor(ComponentMask interest, TaskHandle_t handle) {
    // Called at boot time only — no lock needed (tasks haven't started writing yet)
    if (handle == nullptr) {
        ESP_LOGE(TAG, "registerReactor: null task handle (mask=0x%02lx) ignored", (unsigned long)interest);
        return;
    }
    if (m_reactor_count >= MAX_REACTORS) {
        ESP_LOGE(TAG, "MAX_REACTORS (%zu) exceeded — increase limit", MAX_REACTORS);
        return;
    }
    m_reactors[m_reactor_count++] = { interest, handle };
    ESP_LOGI(TAG, "Reactor registered (mask=0x%02lx, total=%zu)", (unsigned long)interest, m_reactor_count);
}

void EmbeddedSysDb::notifyReactors_locked(ComponentMask changed) {
    uint32_t changed_comp = changed & 0xFFFF0000;
    for (size_t i = 0; i < m_reactor_count; ++i) {
        if (m_reactors[i].mask & changed_comp) {
            xTaskNotify(m_reactors[i].handle, changed, eSetBits);
        }
    }
}

ComponentMask EmbeddedSysDb::diffState(const SystemState& old_s, const SystemState& new_s) {
    return SysDbCodec::diffState(old_s, new_s);
}
