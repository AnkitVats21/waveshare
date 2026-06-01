#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_afe_sr_iface.h"

#include "hal/interfaces/IAudioFeedSource.h"
#include "app/wake_word/IWakeWordListener.h"
#include "services/BufferManager.h"

/**
 * @brief Wake-word detector — pure engine, no Board/EventBus coupling.
 *
 * Architecture:
 *  - feedTask (Core 0): reads 4-ch audio via IAudioFeedSource, feeds AFE.
 *  - detectTask (Core 1): fetches AFE results, runs WakeNet, fires listener
 *    callbacks. All state transitions happen here.
 *
 * State machine (internal, driven by detectTask):
 *   IDLE ──(wake word)──► LISTENING
 *   LISTENING ──(speech)──► STREAMING   (starts writing to MIC_TX_BUF)
 *   STREAMING ──(VAD silence timeout)──► IDLE
 *   STREAMING ──(speech while assistant active)──► BARGE_IN ──► STREAMING
 *
 * Lifecycle:
 *   1. Call setFeedSource() with the Board's IAudioFeedSource implementation.
 *   2. Call setListener() with the AudioService instance.
 *   3. Call begin(). The detector initialises AFE and launches tasks.
 *   4. Call stop() to request teardown. stop() blocks until both tasks exit
 *      (semaphore-based, safe for hardware reinit).
 */
class WakeWordDetector {
public:
    static WakeWordDetector &getInstance();

    /**
     * @brief Inject the hardware audio source. Call before begin().
     */
    void setFeedSource(IAudioFeedSource *source) { m_feed_source = source; }

    /**
     * @brief Inject the event listener. Call before begin().
     */
    void setListener(IWakeWordListener *listener) { m_listener = listener; }

    /**
     * @brief Initialise the AFE engine and launch feed + detect tasks.
     * @return true on success.
     */
    bool begin();

    /**
     * @brief Request stop and block until both tasks have exited.
     * Safe to call before AudioHal::reinit().
     */
    void stop();

    // ---- State accessors (called from AudioService, event-task context) ----
    bool isRunning()         const { return m_task_flag != 0; }
    void setVadDeferred(bool d)    { m_vad_deferred = d; }
    bool isVadDeferred()     const { return m_vad_deferred; }
    void setAssistantActive(bool a);
    bool isAssistantActive() const { return m_assistant_active; }
    bool isStreamingActive() const { return m_streaming_active; }
    void stopStreaming();

    /** Pause hardware reads in feedTask (call before AudioHal::reinit). */
    void pauseHardware()  { m_hw_valid = false; }
    /** Resume hardware reads after AudioHal::reinit completes. */
    void resumeHardware() { m_hw_valid = true;  }

private:
    WakeWordDetector();
    ~WakeWordDetector();

    // FreeRTOS task bridges
    static void feedTaskBridge(void *arg);
    static void detectTaskBridge(void *arg);

    // Task bodies
    void feedTask(esp_afe_sr_data_t *afe_data);
    void detectTask(esp_afe_sr_data_t *afe_data);

    // ---- Injected dependencies ----
    IAudioFeedSource  *m_feed_source = nullptr;
    IWakeWordListener *m_listener    = nullptr;

    // ---- AFE engine ----
    const esp_afe_sr_iface_t *m_afe_handle = nullptr;
    esp_afe_sr_data_t        *m_afe_data   = nullptr;

    // ---- Lifecycle ----
    volatile int  m_task_flag     = 0; ///< 1 = tasks should run, 0 = stop
    SemaphoreHandle_t m_feed_done   = nullptr; ///< signalled when feedTask exits
    SemaphoreHandle_t m_detect_done = nullptr; ///< signalled when detectTask exits

    // ---- State flags (volatile — written by detectTask, read by AudioService) ----
    volatile bool m_streaming_active     = false;
    volatile bool m_vad_deferred         = false;
    volatile bool m_assistant_active     = false;
    volatile bool m_interruption_triggered = false;
    volatile bool m_hw_valid             = true;

    // ---- Config ----
    static constexpr int VAD_SILENCE_TIMEOUT_MS = 3000;
    static constexpr const char *TAG = "WakeWord";
};
