#include "GeminiPCMDrainerTask.h"
#include "app/audio/SpeakerPlayback.h"
#include "app/audio/BtSpeakerPlaybackTask.h"
#include "common/AppLogger.h"
#include "common/thread_config.h"
#include "services/BufferManager.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ── Buffer definition ─────────────────────────────────────────────────────────
// 512 KB — absorbs the burst of Gemini audio per turn.  With the backpressure
// chain in place (processIncomingFrame blocks on portMAX_DELAY, WS ring-buffer
// timeout raised to 500 ms), this buffer rarely approaches its limit.
DEFINE_BUFFER(GEMINI_PCM_BUF, "gemini_pcm", 1024 * 1024)

namespace {

/**
 * @brief Upscales 24 kHz Mono 16-bit PCM to 48 kHz Stereo 16-bit PCM.
 *
 * Linearly interpolates samples to double sample rate from 24kHz -> 48kHz
 * and duplicates channels for stereo output.
 */
static void upsample_24k_mono_to_48k_stereo(const int16_t* src, size_t src_samples, int16_t* dst) {
    for (size_t i = 0; i < src_samples; i++) {
        int16_t s0 = src[i];
        int16_t s1 = (i + 1 < src_samples) ? src[i + 1] : s0;
        int16_t s_mid = static_cast<int16_t>((static_cast<int32_t>(s0) + static_cast<int32_t>(s1)) >> 1);

        // Frame 2*i: s0 for L & R
        dst[4 * i + 0] = s0;
        dst[4 * i + 1] = s0;
        // Frame 2*i + 1: s_mid for L & R
        dst[4 * i + 2] = s_mid;
        dst[4 * i + 3] = s_mid;
    }
}

} // namespace

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
    if (BufferManager::getInstance().handle(Buffers::BT_SPK_BUF)) {
        BufferManager::getInstance().flush(Buffers::BT_SPK_BUF);
    }
}

// ── Run loop ──────────────────────────────────────────────────────────────────

void GeminiPCMDrainerTask::run() {
    LOGI_AUDIO("GeminiPCMDrainerTask started on Core %d", xPortGetCoreID());
    auto& bm = BufferManager::getInstance();

    constexpr size_t MAX_BT_UPSAMPLE_BYTES = 8192;
    int16_t* bt_upsample_buf = (int16_t*)heap_caps_malloc(
        MAX_BT_UPSAMPLE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

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

            // Also up-scale to 48 kHz stereo and send to BT_SPK_BUF if active
            if (bm.handle(Buffers::BT_SPK_BUF) != nullptr) {
                size_t src_samples = rx_bytes / sizeof(int16_t);
                size_t req_bytes = src_samples * 4 * sizeof(int16_t);
                if (bt_upsample_buf && req_bytes <= MAX_BT_UPSAMPLE_BYTES) {
                    upsample_24k_mono_to_48k_stereo((const int16_t*)rx_ptr, src_samples, bt_upsample_buf);
                    bm.send(Buffers::BT_SPK_BUF, bt_upsample_buf, req_bytes, pdMS_TO_TICKS(10));
                }
            }
        }

        bm.returnItem(Buffers::GEMINI_PCM_BUF, rx_ptr);
        taskYIELD(); // give SpeakerPlaybackTask a chance to drain what we just sent
    }

    if (bt_upsample_buf) {
        heap_caps_free(bt_upsample_buf);
    }

    LOGI_AUDIO("GeminiPCMDrainerTask exiting.");
}

