#include "GeminiPCMDrainerTask.h"
#include "app/audio/SpeakerPlayback.h"
#include "common/AppLogger.h"
#include "common/thread_config.h"
#include "services/BufferManager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ── Buffer definition ─────────────────────────────────────────────────────────
// 512 KB — absorbs the burst of Gemini audio per turn.  With the backpressure
// chain in place (processIncomingFrame blocks on portMAX_DELAY, WS ring-buffer
// timeout raised to 500 ms), this buffer rarely approaches its limit.
DEFINE_BUFFER(GEMINI_PCM_BUF, "gemini_pcm", 1024 * 1024)

// ── Singleton ─────────────────────────────────────────────────────────────────

GeminiPCMDrainerTask& GeminiPCMDrainerTask::getInstance() {
    static GeminiPCMDrainerTask instance;
    return instance;
}

GeminiPCMDrainerTask::GeminiPCMDrainerTask()
    : TaskBase({
          "gemini_drainer",
          ThreadConfig::StackSize::STACK_NORMAL,
          // One tick below SPEAKER_PLAYBACK so the speaker always gets CPU first
          static_cast<UBaseType_t>(ThreadConfig::Priority::SPEAKER_PLAYBACK - 1),
          ThreadConfig::CORE_AUDIO
      }) {}

// ── Control API ───────────────────────────────────────────────────────────────

void GeminiPCMDrainerTask::suspend() {
    m_suspended = true;
}

void GeminiPCMDrainerTask::resume() {
    m_suspended = false;
}

void GeminiPCMDrainerTask::suspendAndFlush() {
    m_suspended = true;
    // Discard any audio that arrived before/during the suspend
    BufferManager::getInstance().flush(Buffers::GEMINI_PCM_BUF);
}

// ── Run loop ──────────────────────────────────────────────────────────────────

void GeminiPCMDrainerTask::run() {
    LOGI_AUDIO("GeminiPCMDrainerTask started on Core %d", xPortGetCoreID());
    auto& bm = BufferManager::getInstance();

    while (m_running) {
        if (m_suspended) {
            vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
            continue;
        }

        size_t rx_bytes = 0;
        void* rx_ptr = bm.receive(Buffers::GEMINI_PCM_BUF,
                                  &rx_bytes,
                                  pdMS_TO_TICKS(IDLE_POLL_MS),
                                  DRAIN_CHUNK_BYTES);

        if (rx_ptr == nullptr || rx_bytes == 0) {
            // Buffer empty — stay blocked; loop checks m_suspended and m_running
            continue;
        }

        // Re-check suspended flag while we held the item
        if (!m_suspended) {
            bool sent = bm.send(Buffers::SPK_RX_BUF,
                                rx_ptr,
                                rx_bytes,
                                pdMS_TO_TICKS(DRAIN_SEND_TIMEOUT_MS));
            if (!sent) {
                LOGW_AUDIO("GeminiPCMDrainer: SPK_RX_BUF full, dropping %zu bytes", rx_bytes);
            }
        }

        bm.returnItem(Buffers::GEMINI_PCM_BUF, rx_ptr);
        taskYIELD(); // give SpeakerPlaybackTask a chance to drain what we just sent
    }

    LOGI_AUDIO("GeminiPCMDrainerTask exiting.");
}
