#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_afe_sr_iface.h"

#include "hal/audio/IAudioFeedSource.h"
#include "app/wake_word/IWakeWordListener.h"
#include "services/BufferManager.h"

/**
 * @brief Wake-word engine — pure AFE driver, no Board/EventBus/SysDb coupling.
 *
 * Renamed from WakeWordDetector. Dependency injection:
 *   - IAudioFeedSource* → now supplied as AudioHal& (which implements it)
 *   - IWakeWordListener* → AudioService (still implements it)
 *
 * Architecture unchanged — two dedicated FreeRTOS tasks:
 *   feedTask  (Core 1): reads 4-ch audio via IAudioFeedSource, feeds AFE.
 *   detectTask(Core 1): fetches AFE results, runs WakeNet, fires callbacks.
 *
 * Lifecycle:
 *   1. setFeedSource() with AudioHal& (implements IAudioFeedSource).
 *   2. setListener()   with AudioService instance (implements IWakeWordListener).
 *   3. begin()   — initialises AFE and launches tasks.
 *   4. stop()    — blocks until both tasks exit (semaphore handshake).
 */
class WakeWordEngine {
public:
    static WakeWordEngine& getInstance();

    void setFeedSource(IAudioFeedSource* source) { m_feed_source = source; }
    void setListener(IWakeWordListener* listener) { m_listener    = listener; }

    bool begin();
    void stop();

    // State accessors
    bool isRunning()            const { return m_task_flag != 0; }
    void setVadDeferred(bool d)       { m_vad_deferred  = d; }
    bool isVadDeferred()        const { return m_vad_deferred; }
    void setAssistantActive(bool a);
    bool isAssistantActive()    const { return m_assistant_active; }
    bool isStreamingActive()    const { return m_streaming_active; }
    void stopStreaming();

    /** Pause both tasks cleanly before hardware clock switches. */
    void pauseProcessing();
    /** Resume both tasks after hardware is ready. */
    void resumeProcessing();

    // Legacy aliases for call-site compatibility
    void pauseHardware()  { pauseProcessing(); }
    void resumeHardware() { resumeProcessing(); }

private:
    WakeWordEngine();
    ~WakeWordEngine();

    static void feedTaskBridge  (void* arg);
    static void detectTaskBridge(void* arg);

    void feedTask  (esp_afe_sr_data_t* afe_data);
    void detectTask(esp_afe_sr_data_t* afe_data);

    IAudioFeedSource*  m_feed_source = nullptr;
    IWakeWordListener* m_listener    = nullptr;

    const esp_afe_sr_iface_t* m_afe_handle = nullptr;
    esp_afe_sr_data_t*        m_afe_data   = nullptr;

    volatile int      m_task_flag     = 0;
    SemaphoreHandle_t m_feed_done     = nullptr;
    SemaphoreHandle_t m_detect_done   = nullptr;

    EventGroupHandle_t m_audio_event_group = nullptr;
    static constexpr EventBits_t AUDIO_RUNNING_BIT = (1 << 0);

    volatile bool m_streaming_active       = false;
    volatile bool m_vad_deferred           = false;
    volatile bool m_assistant_active       = false;
    volatile bool m_interruption_triggered = false;

    static constexpr int VAD_SILENCE_TIMEOUT_MS = 5000;
    static constexpr const char* TAG = "WakeWordEngine";
};

// Backward-compatibility alias — remove after all callers updated
using WakeWordDetector = WakeWordEngine;
