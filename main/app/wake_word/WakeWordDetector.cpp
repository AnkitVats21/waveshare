#include "app/wake_word/WakeWordDetector.h"

#include <cstring>

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_process_sdkconfig.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include "app/audio/MicCapture.h"  // for Buffers::MIC_TX_BUF
#include "app/audio/SpeakerPlayback.h"  // for Buffers::SPK_RX_BUF
#include "common/AppLogger.h"
#include "services/BufferManager.h"
#include "app/audio/AudioService.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ============================================================================
// Singleton
// ============================================================================

WakeWordDetector::WakeWordDetector() {
    m_feed_done        = xSemaphoreCreateBinary();
    m_detect_done      = xSemaphoreCreateBinary();
    m_audio_event_group = xEventGroupCreate();
    // Start in "running" state so tasks begin processing immediately after begin()
    if (m_audio_event_group) {
        xEventGroupSetBits(m_audio_event_group, AUDIO_RUNNING_BIT);
    }
}

WakeWordDetector::~WakeWordDetector() {
    if (m_feed_done)         vSemaphoreDelete(m_feed_done);
    if (m_detect_done)       vSemaphoreDelete(m_detect_done);
    if (m_audio_event_group) vEventGroupDelete(m_audio_event_group);
}

WakeWordDetector &WakeWordDetector::getInstance() {
    static WakeWordDetector instance;
    return instance;
}

// ============================================================================
// State helpers
// ============================================================================

void WakeWordDetector::setAssistantActive(bool active) {
    m_assistant_active = active;
    if (active) {
        m_interruption_triggered = false;
    }
}

// ============================================================================
// begin() — init AFE + launch tasks
// ============================================================================

bool WakeWordDetector::begin() {
    if (m_task_flag) {
        ESP_LOGW(TAG, "Already running");
        return true;
    }

    if (!m_feed_source) {
        ESP_LOGE(TAG, "No IAudioFeedSource set — call setFeedSource() first");
        return false;
    }
    if (!m_listener) {
        ESP_LOGE(TAG, "No IWakeWordListener set — call setListener() first");
        return false;
    }

    // 1. Load SR models from the 'model' flash partition
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG,
                 "esp_srmodel_init failed — check partitions.csv for 'model' entry");
        return false;
    }

    // 2. Build AFE config (4-channel RMNM, low-cost mode)
    const char  *input_format = m_feed_source->feedInputFormat();
    afe_config_t *afe_config  = afe_config_init(input_format, models,
                                                 AFE_TYPE_SR, AFE_MODE_LOW_COST);
    afe_config->ns_init  = true;
    afe_config->vad_init = true; // needed for VAD-based streaming timeout

    // 3. Create AFE handle + data
    m_afe_handle = esp_afe_handle_from_config(afe_config);
    if (!m_afe_handle) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(afe_config);
        return false;
    }

    esp_afe_sr_data_t *afe_data = m_afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);
    if (!afe_data) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        return false;
    }

    m_task_flag              = 1;
    m_streaming_active       = false;
    m_interruption_triggered = false;
    // Ensure both tasks start in the running state
    if (m_audio_event_group) {
        xEventGroupSetBits(m_audio_event_group, AUDIO_RUNNING_BIT);
    }

    // 4. Launch tasks (detect on Core 1, feed on Core 0)
    xTaskCreatePinnedToCore(detectTaskBridge, "ww_detect", 8 * 1024,
                            afe_data, 5, nullptr, 1);
    xTaskCreatePinnedToCore(feedTaskBridge,   "ww_feed",   8 * 1024,
                            afe_data, 6, nullptr, 1);

    LOGI_SYSTEM("WakeWordDetector started (feed: %s)", input_format);
    return true;
}

// ============================================================================
// stop() — semaphore-based safe teardown
// ============================================================================

void WakeWordDetector::stop() {
    if (!m_task_flag)
        return;

    m_task_flag = 0;
    // Unblock any parked tasks so they can see m_task_flag == 0 and exit cleanly.
    // Without this, a paused task would block forever on xEventGroupWaitBits.
    if (m_audio_event_group) {
        xEventGroupSetBits(m_audio_event_group, AUDIO_RUNNING_BIT);
    }
    ESP_LOGI(TAG, "stop(): waiting for feedTask...");
    xSemaphoreTake(m_feed_done,   pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "stop(): waiting for detectTask...");
    xSemaphoreTake(m_detect_done, pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "stop(): both tasks exited.");
}

// ============================================================================
// FreeRTOS task bridges
// ============================================================================

void WakeWordDetector::feedTaskBridge(void *arg) {
    WakeWordDetector::getInstance().feedTask(
        static_cast<esp_afe_sr_data_t *>(arg));
}

void WakeWordDetector::detectTaskBridge(void *arg) {
    WakeWordDetector::getInstance().detectTask(
        static_cast<esp_afe_sr_data_t *>(arg));
}

// ============================================================================
// feedTask: read 4-ch mic data via IAudioFeedSource → push to AFE
// ============================================================================

void WakeWordDetector::feedTask(esp_afe_sr_data_t *afe_data) {
    int audio_chunksize = m_afe_handle->get_feed_chunksize(afe_data);
    int nch             = m_afe_handle->get_feed_channel_num(afe_data);
    int feed_channel    = m_feed_source->feedChannelCount();

    if (nch != feed_channel) {
        ESP_LOGE(TAG, "AFE channel mismatch: AFE wants %d, source provides %d",
                 nch, feed_channel);
        xSemaphoreGive(m_feed_done);
        vTaskDelete(nullptr);
        return;
    }

    int      buf_bytes = audio_chunksize * (int)sizeof(int16_t) * feed_channel;
    int16_t *i2s_buff  = static_cast<int16_t *>(
        heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM));
    if (!i2s_buff) {
        ESP_LOGE(TAG, "Failed to allocate feed buffer (%d bytes)", buf_bytes);
        xSemaphoreGive(m_feed_done);
        vTaskDelete(nullptr);
        return;
    }

    esp_task_wdt_add(nullptr);

    // Warm-up: ignore the first ~800ms to let mic hardware bias settle
    int warmup_chunks = 50;

    while (m_task_flag) {
        // Use a timed wait so paused tasks still periodically service TWDT.
        EventBits_t bits = xEventGroupWaitBits(m_audio_event_group,
                                               AUDIO_RUNNING_BIT,
                                               pdFALSE,
                                               pdTRUE,
                                               pdMS_TO_TICKS(250));

        if ((bits & AUDIO_RUNNING_BIT) == 0) {
            esp_task_wdt_reset();
            continue;
        }

        // Re-check task flag after waking — stop() may have unblocked us to exit
        if (!m_task_flag) break;

        // Sole hardware reader — MicCaptureTask must stay soft-disabled
        m_feed_source->readFeedData(i2s_buff, buf_bytes);

        if (warmup_chunks > 0) {
            --warmup_chunks;
            std::memset(i2s_buff, 0, buf_bytes);
        }
        // Sending data to AFE SR engine. The feed() call is thread-safe and
        // returns immediately after copying data into the ring buffer.
        m_afe_handle->feed(afe_data, i2s_buff);
        esp_task_wdt_reset();
    }

    heap_caps_free(i2s_buff);
    esp_task_wdt_delete(nullptr);
    xSemaphoreGive(m_feed_done);
    vTaskDelete(nullptr);
}

// ============================================================================
// detectTask: fetch AFE results → WakeNet → fire IWakeWordListener callbacks
//
// MultiNet (command recognition) is intentionally removed — POC code only.
// ============================================================================

void WakeWordDetector::detectTask(esp_afe_sr_data_t *afe_data) {
    m_afe_data = afe_data;
    int fetch_chunksize  = m_afe_handle->get_fetch_chunksize(afe_data);

    const int SILENCE_TIMEOUT_FRAMES =
        (VAD_SILENCE_TIMEOUT_MS * 16000) / (fetch_chunksize * 1000);

    ESP_LOGI(TAG, "detect: chunksize=%d  silence_timeout=%d frames",
             fetch_chunksize, SILENCE_TIMEOUT_FRAMES);

    esp_task_wdt_add(nullptr);

    // Convenience ref to the ring buffer we stream beamformed audio into
    auto &bm = BufferManager::getInstance();

    int silence_frames = 0;

    while (m_task_flag) {
        // Timed wait keeps the task watchdog satisfied while processing is paused.
        EventBits_t bits = xEventGroupWaitBits(m_audio_event_group,
                                               AUDIO_RUNNING_BIT,
                                               pdFALSE,
                                               pdTRUE,
                                               pdMS_TO_TICKS(250));

        if ((bits & AUDIO_RUNNING_BIT) == 0) {
            esp_task_wdt_reset();
            continue;
        }

        // Re-check task flag after waking — stop() may have unblocked us to exit
        if (!m_task_flag) break;

        // Safe to call fetch() — feedTask is guaranteed to be running too
        afe_fetch_result_t *res = m_afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            // Transient starvation on resume edge; back off briefly and retry.
            ESP_LOGW(TAG, "AFE fetch returned null/fail — possible resume edge, retrying");
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // ── Stream AFE-processed audio into MIC_TX_BUF ───────────────────────
        // res->data is the beamformed + AEC mono output (cleaner than raw Mic1).
        // Only write while streaming is active (wake word confirmed) and assistant is quiet.
        bool assistant_talking = m_assistant_active;
        size_t buffered_spk_bytes = bm.getUsedBytes(Buffers::SPK_RX_BUF);
        bool block_mic_capture = assistant_talking || (buffered_spk_bytes > 0);

        if (m_streaming_active && !block_mic_capture && res->data && res->data_size > 0) {
            bm.send(Buffers::MIC_TX_BUF, res->data, res->data_size);
        }

        // ── Wake word detection ───────────────────────────────────────────────
        bool detected = false;
        uint8_t channel = 0;

        if (res->raw_data_channels == 1 &&
            res->wakeup_state == WAKENET_DETECTED) {
            detected = true;
            channel  = (uint8_t)res->trigger_channel_id;
            LOGI_SYSTEM("Wake word detected (1-ch) — streaming started");
        } else if (res->raw_data_channels > 1 &&
                   res->wakeup_state == WAKENET_CHANNEL_VERIFIED) {
            detected = true;
            channel  = (uint8_t)res->trigger_channel_id;
            LOGI_SYSTEM("Wake word detected (ch %d) — streaming started", channel);
        }

        if (detected) {
            m_streaming_active = true;
            // Disable WakeNet dynamically to save huge CPU cycles during active conversation
            m_afe_handle->disable_wakenet(afe_data);
            ESP_LOGI(TAG, "AFE WakeNet suspended (CPU cycles saved!)");

            // Listener (AudioService) handles EventBus, LEDs, mic gain
            m_listener->onWakeWord(channel);
        }

        // ── VAD-based inactivity supervisor ──────────────────────────────────
        if (m_streaming_active && !m_assistant_active) {
            if (res->vad_state == VAD_SPEECH) {
                // Update inactivity timer on active user speech (completely immune to speaker echo)
                AudioService::getInstance().updateActivity();
                silence_frames = 0;
            } else {
                silence_frames++;
                if (silence_frames >= SILENCE_TIMEOUT_FRAMES) {
                    LOGW_AUDIO("VAD: Silence threshold reached (%d ms). Suspending stream.", (int)VAD_SILENCE_TIMEOUT_MS);
                    m_streaming_active = false; // Suspend immediately to stop pump task
                    silence_frames = 0;
                    m_listener->onVadTimeout();
                }
            }
        } else {
            silence_frames = 0;
        }

        esp_task_wdt_reset();
    }

    ESP_LOGI(TAG, "detectTask exiting");
    esp_task_wdt_delete(nullptr);
    xSemaphoreGive(m_detect_done);
    vTaskDelete(nullptr);
}

void WakeWordDetector::stopStreaming() {
    m_streaming_active = false;
    m_interruption_triggered = false;
    m_assistant_active = false;
    m_vad_deferred = false;
    if (m_afe_handle && m_afe_data) {
        m_afe_handle->enable_wakenet(m_afe_data);
        ESP_LOGI(TAG, "stopStreaming(): AFE WakeNet re-armed successfully");
    }
}

// ============================================================================
// pauseProcessing / resumeProcessing — EventGroup-based task gating
// ============================================================================

void WakeWordDetector::pauseProcessing() {
    ESP_LOGI(TAG, "pauseProcessing(): parking feedTask and detectTask...");
    // 1. Drop RUNNING bit — both tasks will block at the top of their loops
    //    the next time they reach xEventGroupWaitBits (within one loop iteration).
    if (m_audio_event_group) {
        xEventGroupClearBits(m_audio_event_group, AUDIO_RUNNING_BIT);
    }
    // 2. Flush the AFE internal ring buffer so stale audio from before the
    //    clock switch doesn't cause pitch/sync artifacts when we resume.
    if (m_afe_handle && m_afe_data) {
        m_afe_handle->reset_buffer(m_afe_data);
    }
    ESP_LOGI(TAG, "pauseProcessing(): pipeline paused (tasks will block on next loop).");
}

void WakeWordDetector::resumeProcessing() {
    ESP_LOGI(TAG, "resumeProcessing(): releasing feedTask and detectTask...");
    // Set RUNNING bit — both tasks unblock simultaneously in the FreeRTOS scheduler
    if (m_audio_event_group) {
        xEventGroupSetBits(m_audio_event_group, AUDIO_RUNNING_BIT);
    }
    ESP_LOGI(TAG, "resumeProcessing(): pipeline resumed.");
}
