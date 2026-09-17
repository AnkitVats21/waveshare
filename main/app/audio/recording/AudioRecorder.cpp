#include "app/audio/recording/AudioRecorder.h"
#include "app/audio/recording/WavPcmEncoder.h"
#include "audio_core/WakeWordEngine.h"
#include "audio_core/AlertPlayer.h"
#include "core_sysdb/EmbeddedSysDb.h"
#include "core_sysdb/BufferManager.h"
#include "core_sysdb/thread_config.h"
#include "services/storage/StorageService.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <ctime>

namespace {
constexpr uint32_t RAW_SAMPLE_RATE        = LOCAL_SAMPLE_RATE; // 32000
constexpr uint32_t RESAMPLED_SAMPLE_RATE  = 16000;

struct HighestIndexCtx {
    int highest = 0;
};

void findHighestIndexCb(const char* filename, void* ctx) {
    auto* c = static_cast<HighestIndexCtx*>(ctx);
    int idx = 0;
    if (sscanf(filename, "rec_%d_", &idx) == 1 && idx > c->highest) {
        c->highest = idx;
    }
}
} // namespace

AudioRecorder& AudioRecorder::getInstance() {
    static AudioRecorder instance;
    return instance;
}

bool AudioRecorder::resolveFilePath(RecordMode mode, char* out_path, size_t out_len) {
    struct stat st;
    if (stat("/sdcard/recordings", &st) != 0) {
        if (mkdir("/sdcard/recordings", 0755) != 0) {
            ESP_LOGW(TAG, "Failed to create /sdcard/recordings (may already exist): %d", errno);
        }
    }

    const char* mode_str = (mode == RecordMode::RAW) ? "raw" : "rsmp";

    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (tmv.tm_year + 1900 >= 2024) {
        snprintf(out_path, out_len, "/sdcard/recordings/rec_%04d%02d%02d_%02d%02d%02d_%s.wav",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, mode_str);
        return true;
    }

    HighestIndexCtx ctx;
    Services::StorageService::getInstance().listFiles("/sdcard/recordings", ".wav", findHighestIndexCb, &ctx);
    snprintf(out_path, out_len, "/sdcard/recordings/rec_%03d_%s.wav", ctx.highest + 1, mode_str);
    return true;
}

bool AudioRecorder::startRecording(RecordMode mode) {
    if (m_active) {
        ESP_LOGW(TAG, "startRecording(): already recording, ignoring");
        return false;
    }

    if (!resolveFilePath(mode, m_active_path, sizeof(m_active_path))) {
        ESP_LOGE(TAG, "startRecording(): failed to resolve output path");
        return false;
    }

    m_stream = Services::StorageService::getInstance().openStream(m_active_path, "wb");
    if (!m_stream) {
        ESP_LOGE(TAG, "startRecording(): failed to open %s for writing", m_active_path);
        return false;
    }

    auto* encoder = new WavPcmEncoder();
    uint32_t sample_rate;
    int channels;
    if (mode == RecordMode::RAW) {
        bool all_channels = EmbeddedSysDb::getInstance().snapshot().audio.record_all_mic_channels;
        sample_rate = RAW_SAMPLE_RATE;
        channels    = all_channels ? 4 : 1;
    } else {
        sample_rate = RESAMPLED_SAMPLE_RATE;
        channels    = 1;
    }

    if (!encoder->open(m_stream, sample_rate, channels, 16)) {
        ESP_LOGE(TAG, "startRecording(): failed to write WAV header");
        Services::StorageService::getInstance().closeStream(m_stream);
        m_stream = nullptr;
        delete encoder;
        return false;
    }
    m_encoder = encoder;
    m_mode    = mode;
    m_channels = channels;
    m_next_expected_seq  = 0;
    m_padded_chunk_count = 0;
    m_stage_used         = 0;
    if (!m_stage_buf) {
        m_stage_buf = static_cast<uint8_t*>(heap_caps_malloc(STAGE_BYTES, MALLOC_CAP_SPIRAM));
    }

    m_writer_running = true;
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        writerTaskThunk, "audio_rec_writer", ThreadConfig::StackSize::STACK_STORAGE, this,
        ThreadConfig::Priority::STORAGE_IO, &m_writer_task, ThreadConfig::CORE_STORAGE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        ret = xTaskCreatePinnedToCore(
            writerTaskThunk, "audio_rec_writer", ThreadConfig::StackSize::STACK_STORAGE, this,
            ThreadConfig::Priority::STORAGE_IO, &m_writer_task, ThreadConfig::CORE_STORAGE);
    }
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "startRecording(): failed to spawn writer task");
        m_writer_running = false;
        m_encoder->finalize();
        Services::StorageService::getInstance().closeStream(m_stream);
        m_stream = nullptr;
        delete m_encoder;
        m_encoder = nullptr;
        return false;
    }

    m_deadline_ticks = xTaskGetTickCount() + pdMS_TO_TICKS(MAX_DURATION_MS);
    m_active = true;

    if (mode == RecordMode::RAW) {
        bool all_channels = EmbeddedSysDb::getInstance().snapshot().audio.record_all_mic_channels;
        WakeWordEngine::getInstance().startRawRecording(
            all_channels ? WakeWordEngine::RecordChannels::ALL : WakeWordEngine::RecordChannels::MIC1_ONLY);
    } else {
        WakeWordEngine::getInstance().startResampledRecording();
    }
    WakeWordEngine::getInstance().setWakeWordSuppressed(true);
    AlertPlayer::getInstance().playAlert(ALERT_WAKE_CONFIRM);

    ESP_LOGI(TAG, "Recording started: %s (mode=%s, rate=%u, ch=%d)",
             m_active_path, (mode == RecordMode::RAW ? "RAW" : "RESAMPLED"), sample_rate, channels);
    return true;
}

void AudioRecorder::stopRecording(StopReason reason) {
    // Only the MANUAL path (called from outside the writer task, e.g.
    // KeyService) reaches here — the writer task handles its own AUTO_CAP
    // stop inline in runWriterTaskLoop() to avoid waiting on itself below.
    if (!m_active) return;
    m_active = false;

    WakeWordEngine::getInstance().stopRecordingTap();
    WakeWordEngine::getInstance().setWakeWordSuppressed(false);

    // Signal the writer task to finalize + exit via an EOF_STREAM chunk.
    RecordChunkHeader eof{ RecordChunkType::EOF_STREAM, 0, 0 };
    BufferManager::getInstance().send(Buffers::RECORD_TX_BUF, &eof, sizeof(eof), pdMS_TO_TICKS(100));

    while (m_writer_task != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    AlertPlayer::getInstance().playAlert(ALERT_SESSION_END);
    ESP_LOGI(TAG, "Recording stopped (%s): %s",
             reason == StopReason::MANUAL ? "manual" : "auto-cap", m_active_path);
}

void AudioRecorder::writerTaskThunk(void* arg) {
    static_cast<AudioRecorder*>(arg)->runWriterTaskLoop();
}

void AudioRecorder::appendToStage(const int16_t* data, size_t frame_count) {
    size_t bytes = frame_count * (size_t)m_channels * sizeof(int16_t);
    if (!m_stage_buf || bytes > STAGE_BYTES) {
        // Fallback (shouldn't normally happen: chunks are always far smaller
        // than STAGE_BYTES) — write straight through rather than drop data.
        m_encoder->writeSamples(data, frame_count);
        return;
    }
    if (m_stage_used + bytes > STAGE_BYTES) {
        flushStage();
    }
    std::memcpy(m_stage_buf + m_stage_used, data, bytes);
    m_stage_used += bytes;
}

void AudioRecorder::flushStage() {
    if (m_stage_used == 0) return;
    size_t frame_count = m_stage_used / ((size_t)m_channels * sizeof(int16_t));
    m_encoder->writeSamples(reinterpret_cast<const int16_t*>(m_stage_buf), frame_count);
    m_stage_used = 0;
}

void AudioRecorder::runWriterTaskLoop() {
    auto& bm = BufferManager::getInstance();
    bool auto_capped = false;

    while (m_writer_running) {
        // 10-minute auto-cap check. Handled inline (rather than routing
        // through stopRecording()) because that function blocks waiting for
        // this very task to exit — calling it from here would deadlock.
        if (xTaskGetTickCount() >= m_deadline_ticks) {
            ESP_LOGI(TAG, "Writer task: 10-minute cap reached, auto-stopping");
            auto_capped = true;
            m_writer_running = false;
            m_active = false;
            WakeWordEngine::getInstance().stopRecordingTap();
            WakeWordEngine::getInstance().setWakeWordSuppressed(false);
            break;
        }

        size_t rx_bytes = 0;
        void* rx_ptr = bm.receive(Buffers::RECORD_TX_BUF, &rx_bytes, pdMS_TO_TICKS(100));
        if (rx_ptr == nullptr) continue;

        auto* hdr = reinterpret_cast<RecordChunkHeader*>(rx_ptr);
        if (hdr->type == RecordChunkType::EOF_STREAM) {
            bm.returnItem(Buffers::RECORD_TX_BUF, rx_ptr);
            break;
        }

        if (hdr->size > 0) {
            const int16_t* payload = reinterpret_cast<const int16_t*>(
                reinterpret_cast<uint8_t*>(rx_ptr) + sizeof(RecordChunkHeader));
            size_t frame_count = hdr->size / (sizeof(int16_t) * (size_t)m_channels);

            // Gap detection: seq is a monotonic per-chunk counter incremented
            // by the producer even when a send() is dropped. If we skipped
            // one or more sequence numbers, pad with silence for exactly
            // that many chunks' worth of frames instead of splicing this
            // chunk directly onto whatever came before it — keeps both the
            // recorded duration and the audible result honest.
            if (hdr->seq > m_next_expected_seq) {
                uint32_t missing = hdr->seq - m_next_expected_seq;
                if (m_silence_buf_frames != frame_count) {
                    delete[] m_silence_buf;
                    m_silence_buf = new int16_t[frame_count * m_channels]();
                    m_silence_buf_frames = frame_count;
                }
                for (uint32_t i = 0; i < missing; ++i) {
                    appendToStage(m_silence_buf, frame_count);
                }
                m_padded_chunk_count += missing;
            }
            m_next_expected_seq = hdr->seq + 1;

            appendToStage(payload, frame_count);
        }
        bm.returnItem(Buffers::RECORD_TX_BUF, rx_ptr);
    }

    if (m_silence_buf) {
        delete[] m_silence_buf;
        m_silence_buf = nullptr;
        m_silence_buf_frames = 0;
    }

    ESP_LOGI(TAG, "Writer task: %u chunk(s) dropped upstream, %u silence-padded",
             (unsigned)WakeWordEngine::getInstance().recordingDropCount(),
             (unsigned)m_padded_chunk_count);

    // Drain any remaining chunks non-blockingly before finalizing, so a
    // straggling item left in the ring buffer doesn't get picked up by the
    // *next* recording session.
    bm.flush(Buffers::RECORD_TX_BUF);
    flushStage();

    if (m_encoder) {
        m_encoder->finalize();
        delete m_encoder;
        m_encoder = nullptr;
    }
    if (m_stream) {
        Services::StorageService::getInstance().closeStream(m_stream);
        m_stream = nullptr;
    }

    if (auto_capped) {
        AlertPlayer::getInstance().playAlert(ALERT_SESSION_END);
        ESP_LOGI(TAG, "Recording stopped (auto-cap): %s", m_active_path);
    }

    m_writer_running = false;
    m_writer_task = nullptr;
    vTaskDelete(nullptr);
}
