#include "app/audio/recording/AudioRecorder.h"
#include "app/audio/recording/OggOpusEncoder.h"
#include "audio_core/WakeWordEngine.h"
#include "audio_core/AlertPlayer.h"
#include "core_sysdb/EmbeddedSysDb.h"
#include "core_sysdb/BufferManager.h"
#include "core_sysdb/thread_config.h"
#include "sd_storage/Fs.h"
#include "services/storage/RecordingsDatabase.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include <cstring>
#include <cstdio>
#include <ctime>

namespace {
constexpr const char* RECORDINGS_DIR = "/sdcard/recordings";
constexpr uint32_t STEREO_INPUT_RATE    = LOCAL_SAMPLE_RATE; // 32000, the mic feed
constexpr uint32_t PROCESSED_INPUT_RATE = 16000;             // AFE output

// 24 kHz keeps sound up to 12 kHz; the 32 kHz feed is resampled down.
// Core 0 cost, measured (CELT only): processed ~16% at complexity 3. Stereo
// at complexity 0 is ~41% at 24 kHz and ~33% at 16 kHz; with music playing
// (core 0 ~57% alone) 24 kHz saturates the core and 16 kHz reaches ~78%, so
// stereo drops to 16 kHz when music is playing at the start.
constexpr OggOpusEncoder::Config STEREO_OPUS            = {24000, 48000, false, 0};
constexpr OggOpusEncoder::Config STEREO_OPUS_WITH_MUSIC = {16000, 32000, false, 0};
constexpr OggOpusEncoder::Config PROCESSED_OPUS         = {16000, 24000, true, 3};

bool musicActive() {
    auto state = EmbeddedSysDb::getInstance().snapshot().media.state;
    return state == MediaPlaybackState::PLAYING || state == MediaPlaybackState::BUFFERING ||
           state == MediaPlaybackState::RESOLVING;
}

struct HighestIndexCtx {
    int highest = 0;
};

bool findHighestIndexCb(const sd_storage::DirEntry& entry, void* ctx) {
    auto* c = static_cast<HighestIndexCtx*>(ctx);
    int idx = 0;
    if (sscanf(entry.name, "rec_%d_", &idx) == 1 && idx > c->highest) {
        c->highest = idx;
    }
    return true;
}
} // namespace

AudioRecorder& AudioRecorder::getInstance() {
    static AudioRecorder instance;
    return instance;
}

bool AudioRecorder::resolveFilePath(RecordMode mode, uint32_t encode_rate, char* out_path, size_t out_len) {
    if (!sd_storage::Fs::mkdirs(RECORDINGS_DIR)) {
        return false;
    }

    char mode_str[24];
    snprintf(mode_str, sizeof(mode_str), "%s%uk", mode == RecordMode::STEREO ? "stereo" : "processed",
             (unsigned)(encode_rate / 1000));

    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (tmv.tm_year + 1900 >= 2024) {
        snprintf(out_path, out_len, "%s/rec_%04d%02d%02d_%02d%02d%02d_%s.opus", RECORDINGS_DIR,
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, mode_str);
        return true;
    }

    HighestIndexCtx ctx;
    sd_storage::Fs::list(RECORDINGS_DIR, nullptr, false, findHighestIndexCb, &ctx);
    snprintf(out_path, out_len, "%s/rec_%03d_%s.opus", RECORDINGS_DIR, ctx.highest + 1, mode_str);
    return true;
}

bool AudioRecorder::startRecording(RecordMode mode) {
    if (m_active) {
        ESP_LOGW(TAG, "startRecording(): already recording, ignoring");
        return false;
    }

    bool stereo = mode == RecordMode::STEREO;
    OggOpusEncoder::Config cfg = !stereo ? PROCESSED_OPUS : (musicActive() ? STEREO_OPUS_WITH_MUSIC : STEREO_OPUS);
    uint32_t sample_rate = stereo ? STEREO_INPUT_RATE : PROCESSED_INPUT_RATE;
    int channels = stereo ? 2 : 1;

    if (!resolveFilePath(mode, cfg.encode_rate, m_active_path, sizeof(m_active_path))) {
        ESP_LOGE(TAG, "startRecording(): failed to resolve output path");
        return false;
    }

    m_file = sd_storage::File::open(m_active_path, sd_storage::Mode::Write);
    if (!m_file) {
        ESP_LOGE(TAG, "startRecording(): failed to open %s for writing", m_active_path);
        return false;
    }

    auto* encoder = new OggOpusEncoder(cfg);

    if (!encoder->open(m_file, sample_rate, channels)) {
        ESP_LOGE(TAG, "startRecording(): failed to start the Opus encoder");
        m_file.close();
        sd_storage::Fs::remove(m_active_path);
        delete encoder;
        return false;
    }
    m_encoder = encoder;
    m_mode    = mode;
    m_channels = channels;
    m_encode_rate = cfg.encode_rate;
    m_next_expected_seq  = 0;
    m_padded_chunk_count = 0;

    // Set before the writer task starts: it checks the deadline immediately.
    time_t now = time(nullptr);
    m_started_epoch  = now >= 1704067200 ? uint32_t(now) : 0;  // 0 if the clock isn't set
    m_started_ticks  = xTaskGetTickCount();
    m_deadline_ticks = m_started_ticks + pdMS_TO_TICKS(MAX_DURATION_MS);
    m_active = true;
    m_writer_running = true;
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        writerTaskThunk, "audio_rec_writer", WRITER_STACK, this,
        ThreadConfig::Priority::STORAGE_IO, &m_writer_task, ThreadConfig::CORE_STORAGE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "startRecording(): failed to spawn writer task");
        m_writer_running = false;
        m_active = false;
        delete m_encoder;
        m_encoder = nullptr;
        m_file.close();
        sd_storage::Fs::remove(m_active_path);
        return false;
    }

    if (stereo) {
        WakeWordEngine::getInstance().startStereoRecording();
    } else {
        WakeWordEngine::getInstance().startResampledRecording();
    }
    WakeWordEngine::getInstance().setWakeWordSuppressed(true);
    AlertPlayer::getInstance().playAlert(ALERT_WAKE_CONFIRM);

    ESP_LOGI(TAG, "Recording started: %s (mode=%s, %u Hz in, %u Hz Opus, %d ch)",
             m_active_path, (stereo ? "STEREO" : "PROCESSED"), (unsigned)sample_rate,
             (unsigned)cfg.encode_rate, channels);
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
                    m_encoder->writeSamples(m_silence_buf, frame_count);
                }
                m_padded_chunk_count += missing;
            }
            m_next_expected_seq = hdr->seq + 1;

            m_encoder->writeSamples(payload, frame_count);
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

    if (m_encoder) {
        m_encoder->finalize();
        auto* opus = static_cast<OggOpusEncoder*>(m_encoder);
        uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - m_started_ticks);
        ESP_LOGI(TAG, "Encoder CPU: %lld ms busy over %u ms recorded (%.1f%% of core %d); writer stack headroom %u B",
                 opus->busyMicros() / 1000, (unsigned)elapsed_ms,
                 elapsed_ms ? opus->busyMicros() / 10.0 / elapsed_ms : 0.0,
                 (int)ThreadConfig::CORE_STORAGE, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        delete m_encoder;
        m_encoder = nullptr;
    }
    m_file.close();
    // Before m_writer_task clears, so a stop request returns with the file listed.
    Services::addRecording(m_active_path, m_started_epoch,
                           m_mode == RecordMode::STEREO ? Services::REC_MODE_STEREO : Services::REC_MODE_PROCESSED,
                           m_encode_rate);

    if (auto_capped) {
        AlertPlayer::getInstance().playAlert(ALERT_SESSION_END);
        ESP_LOGI(TAG, "Recording stopped (auto-cap): %s", m_active_path);
    }

    m_writer_running = false;
    m_writer_task = nullptr;
    // Created WithCaps, so plain vTaskDelete would leak the 32 KB stack.
    vTaskDeleteWithCaps(nullptr);
}
