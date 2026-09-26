#include "services/alerts/AlertLibrary.h"
#include "audio_core/Resampler.h"
#include "core_sysdb/AudioRates.h"
#include "core_sysdb/thread_config.h"
#include "app/media_player/AudioDecoderFactory.h"
#include "sd_storage/Fs.h"
#include "sd_storage/SdCard.h"
#include "services/storage/SystemDatabase.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <cstring>

namespace Services {

namespace {

constexpr const char* TAG = "AlertLibrary";

struct CapsFree { void operator()(void* p) const { heap_caps_free(p); } };
template <typename T>
std::unique_ptr<T, CapsFree> psramArray(size_t count) {
    return std::unique_ptr<T, CapsFree>(
        static_cast<T*>(heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}

// Largest decoder output per call: a 120 ms Opus frame, stereo, 48 kHz.
constexpr size_t CHUNK_SAMPLES = 48000 * 120 / 1000 * 2;

} // namespace

AlertDecode decodeAlertFile(const char* path) {
    AlertDecode out;
    const int64_t t0 = esp_timer_get_time();
    auto fail = [&](const char* why) {
        out.error = why;
        out.clip.reset();
        return out;
    };

    sd_storage::PathInfo info;
    if (!sd_storage::Fs::stat(path, info) || info.is_dir) return fail("file not found");
    if (info.size == 0) return fail("file is empty");
    if (info.size > AlertLibrary::MAX_FILE_BYTES) return fail("file is over 256 KB");

    auto file = psramArray<uint8_t>(info.size);
    auto chunk = psramArray<int16_t>(CHUNK_SAMPLES);
    if (!file || !chunk) return fail("out of PSRAM");
    const size_t len = sd_storage::Fs::readInto(path, file.get(), info.size);
    if (len != info.size) return fail("read failed");

    std::unique_ptr<IAudioDecoder> decoder = AudioDecoderFactory::createDecoder(file.get(), len);
    if (!decoder) return fail("unrecognised audio format");
    decoder->init(MIXER_SAMPLE_RATE, 1);

    // Mono PCM at the source rate, allocated once the rate is known.
    std::unique_ptr<int16_t, CapsFree> mono;
    size_t mono_cap = 0;
    size_t mono_len = 0;
    size_t off = 0;
    // Keeps calling after the input is used up: a decoder may hold whole
    // frames in its own buffer (WebM takes up to 16 KB per call) and hands
    // back one per call. Stops when a call makes no progress.
    while (!out.truncated) {
        size_t consumed = 0, got = 0;
        DecodeResult res = decoder->decode(file.get() + off, len - off, chunk.get(), CHUNK_SAMPLES, consumed, got);
        off += consumed;
        if (got > 0) {
            if (!mono) {
                out.source_rate = decoder->getSourceSampleRate();
                if (out.source_rate < 8000 || out.source_rate > 48000) return fail("unsupported sample rate");
                mono_cap = (size_t)out.source_rate * AlertClip::MAX_SECONDS;
                mono = psramArray<int16_t>(mono_cap);
                if (!mono) return fail("out of PSRAM");
            }
            const int16_t* pcm = chunk.get();
            const bool stereo = decoder->getSourceChannels() == 2;
            const size_t frames = stereo ? got / 2 : got;
            size_t take = frames;
            if (mono_len + take > mono_cap) {
                take = mono_cap - mono_len;
                out.truncated = true;
            }
            int16_t* dst = mono.get() + mono_len;
            for (size_t i = 0; i < take; ++i) {
                dst[i] = stereo ? (int16_t)(((int32_t)pcm[2 * i] + pcm[2 * i + 1]) / 2) : pcm[i];
            }
            mono_len += take;
        }
        if (res == DecodeResult::STREAM_END || res == DecodeResult::ERROR_INVALID_STREAM ||
            res == DecodeResult::ERROR_DECODE_FAILED || res == DecodeResult::OUTPUT_BUFFER_FULL) {
            if (res != DecodeResult::STREAM_END && mono_len == 0) return fail("decode failed");
            break;
        }
        if (consumed == 0 && got == 0) break;   // done, or only an incomplete frame is left
    }
    if (mono_len == 0) return fail("no audio decoded");

    // One resample over the whole clip, so no chunk boundaries to click at.
    size_t dst_len = out.source_rate == MIXER_SAMPLE_RATE
        ? mono_len
        : computeResampledFrames(mono_len, out.source_rate, MIXER_SAMPLE_RATE);
    dst_len = std::min<size_t>(dst_len, (size_t)MIXER_SAMPLE_RATE * AlertClip::MAX_SECONDS);
    out.clip = AlertClip::create(dst_len);
    if (!out.clip) return fail("out of PSRAM");
    if (out.source_rate == MIXER_SAMPLE_RATE) {
        memcpy(out.clip->data(), mono.get(), dst_len * sizeof(int16_t));
    } else {
        LinearResampler().resample(mono.get(), mono_len, out.clip->data(), dst_len, 1);
    }
    // 1 ms in and out guards against a click from a clip that starts or stops
    // off zero; a cut clip gets a 20 ms fade-out.
    out.clip->fadeEdges(MIXER_SAMPLE_RATE / 1000, (out.truncated ? 20 : 1) * MIXER_SAMPLE_RATE / 1000);
    out.decode_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    return out;
}

AlertLibrary& AlertLibrary::getInstance() {
    static AlertLibrary instance;
    return instance;
}

// A unit of work for the loader task. The queue carries a heap-allocated
// shared_ptr, so a waiter that times out never leaves the task a dangling job.
struct AlertLibrary::Job {
    enum Kind : uint8_t { LOAD, LOAD_ALL, DECODE } kind = LOAD;
    AlertType type = ALERT_COUNT;
    std::string path;
    AlertDecode result;
    SemaphoreHandle_t done = nullptr;

    ~Job() { if (done) vSemaphoreDelete(done); }
};

bool AlertLibrary::start() {
    if (m_queue) return true;
    m_queue = xQueueCreate(8, sizeof(std::shared_ptr<Job>*));
    if (!m_queue) return false;
    // The Opus decoder needs a large stack; PSRAM keeps it out of internal RAM.
    TaskHandle_t task = nullptr;
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(taskEntry, "alert_lib", ThreadConfig::StackSize::STACK_LARGE,
                                                    this, ThreadConfig::Priority::LOW, &task,
                                                    ThreadConfig::CORE_STORAGE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to start the loader task");
        return false;
    }
    reload(ALERT_COUNT);
    return true;
}

// Queues the job; with a timeout, waits for the task to finish it.
bool AlertLibrary::submit(const std::shared_ptr<Job>& job, uint32_t timeout_ms) {
    if (!m_queue) return false;
    if (timeout_ms > 0) {
        job->done = xSemaphoreCreateBinary();
        if (!job->done) return false;
    }
    auto* item = new std::shared_ptr<Job>(job);
    if (xQueueSend(m_queue, &item, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        delete item;
        return false;
    }
    return timeout_ms == 0 || xSemaphoreTake(job->done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void AlertLibrary::reload(AlertType type) {
    auto job = std::make_shared<Job>();
    job->kind = type < ALERT_COUNT ? Job::LOAD : Job::LOAD_ALL;
    job->type = type;
    submit(job, 0);
}

bool AlertLibrary::reloadAndWait(AlertType type, uint32_t timeout_ms) {
    if (type >= ALERT_COUNT) return false;
    auto job = std::make_shared<Job>();
    job->type = type;
    return submit(job, timeout_ms);
}

bool AlertLibrary::decodeAndWait(const std::string& path, AlertDecode& out, uint32_t timeout_ms) {
    auto job = std::make_shared<Job>();
    job->kind = Job::DECODE;
    job->path = path;
    if (!submit(job, timeout_ms)) return false;
    out = std::move(job->result);
    return true;
}

void AlertLibrary::applySettings(AlertType type, bool enabled, float gain_db) {
    if (type >= ALERT_COUNT) return;
    gain_db = std::clamp(gain_db, MIN_GAIN_DB, MAX_GAIN_DB);
    AlertPlayer::getInstance().setGainDb(type, gain_db);
    AlertPlayer::getInstance().setEnabled(type, enabled);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_status[type].enabled = enabled;
    m_status[type].gain_db = gain_db;
    m_status[type].custom = true;
}

AlertLibrary::Status AlertLibrary::status(AlertType type) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return type < ALERT_COUNT ? m_status[type] : Status{};
}

void AlertLibrary::taskEntry(void* arg) {
    auto* self = static_cast<AlertLibrary*>(arg);
    std::shared_ptr<Job>* item = nullptr;
    for (;;) {
        if (xQueueReceive(self->m_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        Job& job = **item;
        if (job.kind == Job::LOAD) {
            self->load(job.type);
        } else if (job.kind == Job::DECODE) {
            job.result = decodeAlertFile(job.path.c_str());
        } else {
            const int64_t t0 = esp_timer_get_time();
            for (int i = 0; i < ALERT_COUNT; ++i) self->load(static_cast<AlertType>(i));
            ESP_LOGI(TAG, "All alerts loaded in %lld ms; PSRAM free %u",
                     (long long)((esp_timer_get_time() - t0) / 1000),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
        if (job.done) xSemaphoreGive(job.done);
        delete item;
    }
}

bool AlertLibrary::isValidFileName(const std::string& name) {
    if (name.empty() || name.size() > 64 || name == BUILTIN) return false;
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return false;
    return name[0] != '.';
}

void AlertLibrary::load(AlertType type) {
    AlertPlayer& player = AlertPlayer::getInstance();
    const char* name = alertName(type);

    ndb::system::AlertConfig cfg;
    Status st;
    st.custom = loadAlertConfig(name, cfg);
    st.enabled = cfg.enabled;
    st.gain_db = std::clamp(cfg.gain_db, MIN_GAIN_DB, MAX_GAIN_DB);
    st.setting = cfg.source;

    // "" is the default: <name>.ogg when the card has it, else the tone.
    std::string file;
    if (cfg.source.empty()) {
        std::string def = std::string(name) + ".ogg";
        if (sd_storage::Fs::isFile((std::string(ALERT_DIR) + "/" + def).c_str())) file = def;
    } else if (cfg.source != BUILTIN) {
        file = cfg.source;
    }

    AlertClipPtr clip;
    if (!file.empty()) {
        const std::string path = std::string(ALERT_DIR) + "/" + file;
        AlertDecode d;
        if (!isValidFileName(file)) {
            d.error = "invalid file name";
        } else if (!sd_storage::SdCard::instance().isMounted()) {
            d.error = "SD card not mounted";
        } else {
            d = decodeAlertFile(path.c_str());
        }
        if (d.clip) {
            clip = d.clip;
            st.file = file;
            st.samples = d.clip->size();
            st.decode_ms = d.decode_ms;
            st.truncated = d.truncated;
            ESP_LOGI(TAG, "%s: %u ms of audio from %u Hz, %u bytes, decoded in %u ms%s", name,
                     (unsigned)(st.samples * 1000 / MIXER_SAMPLE_RATE), (unsigned)d.source_rate,
                     (unsigned)(st.samples * sizeof(int16_t)), (unsigned)d.decode_ms,
                     d.truncated ? " (cut to 3 s)" : "");
        } else {
            st.error = d.error;
            ESP_LOGW(TAG, "%s: %s (%s); using the built-in tone", name, d.error.c_str(), path.c_str());
        }
    }
    if (!clip) {
        clip = player.builtin(type);
        if (clip) st.samples = clip->size();
    }
    player.setClip(type, clip);
    player.setGainDb(type, st.gain_db);
    player.setEnabled(type, st.enabled);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_status[type] = st;
}

} // namespace Services
