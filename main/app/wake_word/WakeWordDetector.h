#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_afe_sr_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// ESP-SR event types — used by the detector callback
// ============================================================================

/** @brief Events raised by the wake-word / speech recognition engine. */
typedef enum {
    WAKE_EVT_AWAKEN,       ///< Wake word confirmed; awaken_channel is valid
    WAKE_EVT_CMD,          ///< A speech command was recognised; sr_cmd is valid
    WAKE_EVT_CMD_TIMEOUT   ///< Listening window expired without a command
} wake_word_event_t;

/** @brief Data payload delivered alongside a wake-word event. */
typedef union {
    uint8_t awaken_channel; ///< Channel index that triggered the wake word
    uint8_t sr_cmd;         ///< Recognised command ID
} wake_word_evt_data_t;

/**
 * @brief User callback invoked from the detect task on every SR event.
 *
 * @param event      Event type
 * @param evt_data   Event-specific payload
 * @param user_data  Opaque pointer supplied at registration time
 */
typedef void (*wake_word_callback_t)(wake_word_event_t event,
                                     wake_word_evt_data_t evt_data,
                                     void *user_data);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

// ============================================================================
// C++ WakeWordDetector class
// ============================================================================

/**
 * @brief Wake-word and speech-command detector for the Waveshare ESP32-S3.
 *
 * Wraps the ESP-SR Acoustic Front-End (AFE) + WakeNet + MultiNet pipeline.
 *
 * Architecture:
 *  - feed_Task (Core 0): reads 4-ch audio from AudioHal, feeds AFE
 *  - detect_Task (Core 1): fetches AFE results, runs WakeNet / MultiNet,
 *    fires callbacks via EventBus
 *
 * Integration:
 *  1. Call WakeWordDetector::getInstance().begin() after Board::begin().
 *  2. Subscribe to EventBus APP_EVENTS / AppEvent::WAKE_WORD_DETECTED.
 *  3. Optionally register a raw callback via registerCallback().
 *
 * The detector does NOT start automatically — call begin() explicitly.
 * It can be disabled at compile time via CONFIG_WAVESHARE_WAKEWORD_ENABLE.
 */
class WakeWordDetector {
public:
    static WakeWordDetector &getInstance();

    /**
     * @brief Initialise the AFE engine and launch feed + detect tasks.
     * @return true on success
     */
    bool begin();

    /**
     * @brief Stop the feed and detect tasks and release AFE resources.
     */
    void stop();

    /**
     * @brief Register an optional raw callback (in addition to EventBus).
     *
     * The callback is invoked from the detect task context.
     * Pass nullptr to unregister.
     */
    void registerCallback(wake_word_callback_t cb, void *user_data = nullptr);

    bool isRunning() const { return m_task_flag != 0; }

    /** @brief Pause hardware reads in feedTask (call before AudioHal::reinit). */
    void pauseHardware()  { m_hw_valid = false; }
    /** @brief Resume hardware reads after AudioHal::reinit completes. */
    void resumeHardware() { m_hw_valid = true;  }

    /**
     * @brief Wire in the tx ring buffer so feedTask can stream Mic1 audio.
     * When set, feedTask pushes Mic1 PCM here ONLY while m_streaming_active.
     * MicCaptureTask must remain soft-disabled while detector runs.
     */
    void setStreamRingbuf(RingbufHandle_t buf) { m_stream_ringbuf = buf; }

private:
    WakeWordDetector() = default;
    ~WakeWordDetector() = default;

    // FreeRTOS task bridges
    static void feedTaskBridge(void *arg);
    static void detectTaskBridge(void *arg);

    // Task bodies
    void feedTask(esp_afe_sr_data_t *afe_data);
    void detectTask(esp_afe_sr_data_t *afe_data);

    volatile int                   m_task_flag       = 0;
    const esp_afe_sr_iface_t      *m_afe_handle      = nullptr;
    wake_word_callback_t           m_callback        = nullptr;
    void                          *m_user_data       = nullptr;
    volatile RingbufHandle_t       m_stream_ringbuf  = nullptr;

    // Streaming is gated: feedTask only writes to ring buffer when BOTH
    // m_stream_ringbuf is set AND m_streaming_active is true.
    // detectTask sets m_streaming_active=true on wake word, false on VAD timeout.
    volatile bool                  m_streaming_active = false;

    // Set false before AudioHal::reinit(), true after — prevents feedTask
    // from calling esp_codec_dev_read on freed/invalid handles.
    volatile bool                  m_hw_valid         = true;

    // VAD silence timeout: stop streaming after this many ms of silence
    static constexpr int VAD_SILENCE_TIMEOUT_MS = 3000;

    static constexpr const char *TAG = "WakeWord";
};

#endif // __cplusplus
