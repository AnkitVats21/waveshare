#pragma once

#include "common/TaskBase.h"
#include "common/thread_config.h"
#include "hal/audio/BtSpeakerHal.h"
#include "services/BufferManager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Declare ring buffer for BT Speaker output (32 KB in PSRAM/internal memory)
DECLARE_BUFFER(BT_SPK_BUF, "bt_spk", 32 * 1024)

class BtSpeakerPlaybackTask : public TaskBase {
public:
    BtSpeakerPlaybackTask()
        : TaskBase({
              "bt_spk_playback",
              8 * 1024,
              ThreadConfig::Priority::SPEAKER_PLAYBACK,
              ThreadConfig::CORE_AUDIO
          }) {}

    ~BtSpeakerPlaybackTask() override = default;

    /**
     * @brief Start the Bluetooth speaker playback task.
     * @param hal Pointer to initialized BtSpeakerHal instance.
     */
    void start(BtSpeakerHal* hal);

    /**
     * @brief Stop the playback task.
     */
    void stop() override;

    void setPaused(bool paused) { m_paused = paused; }
    bool isPaused() const { return m_paused; }

protected:
    void run() override;

private:
    static constexpr uint32_t DRAIN_PERIOD_MS = 20; // 20 ms frames
    static constexpr size_t MAX_CHUNK_SAMPLES = 2048;
    static constexpr size_t MAX_CHUNK_BYTES   = MAX_CHUNK_SAMPLES * sizeof(int16_t);

    BtSpeakerHal* m_hal = nullptr;
    volatile bool m_paused = false;
    static constexpr const char* TAG = "BtSpeakerPlaybackTask";
};
