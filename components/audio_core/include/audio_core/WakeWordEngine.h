#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_afe_sr_iface.h"

#include "audio_core/IAudioFeedSource.h"
#include "audio_core/IWakeWordListener.h"
#include "core_sysdb/BufferManager.h"

// Ring buffer for streaming raw/resampled PCM out to the AudioRecorder writer
// task. Declared here (owning component: audio_core), 96KB PSRAM, NOSPLIT type
// since each item carries an AudioChunkHeader prefix that must not be split
// across a wraparound boundary.
// 320KB gives RAW mode (~4KB/chunk) ~1.9s of headroom against SD-write
// stalls, vs. the original 96KB's ~384ms — see AudioRecorder for how any
// drop that still happens gets silence-padded rather than spliced audibly.
DECLARE_BUFFER(RECORD_TX_BUF, "record_tx", 320 * 1024)

// Minimal chunk framing for RECORD_TX_BUF items (kept local to audio_core to
// avoid a dependency on media_player's PlayerTypes.h, which defines the same
// concept under the same names for a different ring buffer).
enum class RecordChunkType : uint8_t { DATA, EOF_STREAM };
struct RecordChunkHeader {
    RecordChunkType type;
    uint32_t        size; // payload bytes following this header
    uint32_t        seq;  // monotonic per-chunk counter (incremented even on
                           // drop) — lets the writer detect a gap and pad it
                           // with silence instead of splicing non-adjacent
                           // audio together.
};

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

    /**
     * @brief Suppress/re-arm WakeNet detection only (mic feed, AEC, and AFE
     * processing keep running). Unlike pauseProcessing(), this does NOT touch
     * the I2S RX DMA feed — used by AudioRecorder so a recording session can
     * disable wake-word triggering without starving itself of audio.
     */
    void setWakeWordSuppressed(bool suppressed);
    bool isWakeWordSuppressed() const { return m_wake_word_suppressed; }

    /** Recording taps — mutually exclusive, driven by AudioRecorder. */
    enum class RecordChannels { ALL, MIC1_ONLY };
    void startRawRecording(RecordChannels channels);
    void startResampledRecording();
    void stopRecordingTap();
    uint32_t recordingDropCount() const { return m_recording_drop_count; }

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
    volatile bool m_wake_word_suppressed   = false;

    // Recording tap state — set by AudioRecorder, read by feedTask/detectTask.
    volatile bool          m_recording_raw_active        = false;
    volatile bool          m_recording_resampled_active  = false;
    RecordChannels         m_recording_raw_channels      = RecordChannels::ALL;
    volatile uint32_t       m_recording_drop_count       = 0;
    // Scratch buffers for framing recorded chunks — allocated once per task
    // (in feedTask/detectTask, alongside their existing SPIRAM buffers) the
    // first time each tap is actually needed, sized to that task's own
    // known worst case. Not touched outside feedTask/detectTask.
    uint8_t *m_raw_tap_scratch        = nullptr;
    uint8_t *m_resampled_tap_scratch  = nullptr;
    uint32_t m_raw_tap_seq            = 0;
    uint32_t m_resampled_tap_seq      = 0;

    static constexpr int VAD_SILENCE_TIMEOUT_MS = 3000;
    static constexpr const char* TAG = "WakeWordEngine";
};

// Backward-compatibility alias — remove after all callers updated
using WakeWordDetector = WakeWordEngine;
