#pragma once

#include "core_sysdb/AudioRates.h"
#include "audio_core/AlertPlayer.h"
#include "audio_core/SpeakerPlayback.h"
#include "audio_core/Resampler.h"
#include "core_sysdb/BufferManager.h"
#include "app/media_player/AudioDecoderFactory.h"
#include "sd_storage/File.h"
#include "sd_storage/Fs.h"
#include "sd_storage/SdCard.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <memory>

class AlertFileDecoder : public IAlertFileDecoder {
public:
    static AlertFileDecoder& getInstance() {
        static AlertFileDecoder instance;
        return instance;
    }

    // Returns true only if some audio was decoded and queued, so the caller
    // falls back to its built-in tone for a missing or undecodable file.
    bool playAlertFile(const char* path) override {
        if (!path || !sd_storage::SdCard::instance().isMounted()) {
            return false;
        }
        if (!sd_storage::Fs::isFile(path)) {
            if (sd_storage::Fs::isFile("/sdcard/media/alert/alert.ogg")) {
                path = "/sdcard/media/alert/alert.ogg";
            } else {
                return false;
            }
        }

        sd_storage::File f = sd_storage::File::open(path, sd_storage::Mode::Read);
        if (!f) return false;

        ESP_LOGI("AlertFileDecoder", "Playing custom alert: %s", path);

        constexpr size_t READ_BUF_SIZE = 1024;
        constexpr size_t MAX_SAMPLES = 4096;
        struct CapsFree { void operator()(void* p) const { heap_caps_free(p); } };
        auto psram = [](size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); };
        std::unique_ptr<uint8_t, CapsFree> read_mem(static_cast<uint8_t*>(psram(READ_BUF_SIZE)));
        std::unique_ptr<int16_t, CapsFree> pcm_mem(static_cast<int16_t*>(psram(MAX_SAMPLES * sizeof(int16_t))));
        std::unique_ptr<int16_t, CapsFree> resample_mem(static_cast<int16_t*>(psram(MAX_SAMPLES * sizeof(int16_t))));
        uint8_t* read_buf = read_mem.get();
        int16_t* pcm_buf = pcm_mem.get();
        int16_t* resample_buf = resample_mem.get();

        if (!read_buf || !pcm_buf || !resample_buf) {
            ESP_LOGE("AlertFileDecoder", "Failed to allocate memory for alert decoding");
            return false;
        }

        std::unique_ptr<IAudioDecoder> decoder;
        size_t payload_len = 0;
        size_t current_offset = 0;
        size_t samples_queued = 0;
        auto& bm = BufferManager::getInstance();

        while (true) {
            if (payload_len == 0 || current_offset >= payload_len) {
                payload_len = f.read(read_buf, READ_BUF_SIZE);
                current_offset = 0;
                if (payload_len == 0) break; // EOF
            }

            if (!decoder) {
                decoder = AudioDecoderFactory::createDecoder(read_buf + current_offset, payload_len - current_offset);
                if (decoder) {
                    decoder->init(MIXER_SAMPLE_RATE, 1);
                } else {
                    ESP_LOGW("AlertFileDecoder", "Unrecognised audio format: %s", path);
                    break;
                }
            }

            size_t bytes_consumed = 0;
            size_t samples_decoded = 0;
            DecodeResult res = decoder->decode(
                read_buf + current_offset, payload_len - current_offset,
                pcm_buf, MAX_SAMPLES,
                bytes_consumed, samples_decoded
            );

            if (res == DecodeResult::STREAM_END || 
                res == DecodeResult::ERROR_INVALID_STREAM || 
                res == DecodeResult::ERROR_DECODE_FAILED) {
                if (samples_decoded == 0) {
                    break;
                }
            }

            if (samples_decoded > 0) {
                uint8_t src_channels = decoder->getSourceChannels();
                int16_t* pcm_mono = pcm_buf;
                size_t mono_samples = samples_decoded;

                if (src_channels == 2) {
                    mono_samples = samples_decoded / 2;
                    for (size_t i = 0; i < mono_samples; ++i) {
                        int32_t mix = (static_cast<int32_t>(pcm_buf[2 * i]) + static_cast<int32_t>(pcm_buf[2 * i + 1])) / 2;
                        pcm_buf[i] = static_cast<int16_t>(mix);
                    }
                }

                uint32_t src_rate = decoder->getSourceSampleRate();
                uint32_t dst_rate = MIXER_SAMPLE_RATE;
                size_t resampled_count = computeResampledFrames(mono_samples, src_rate, dst_rate);
                if (resampled_count > MAX_SAMPLES) resampled_count = MAX_SAMPLES;

                LinearResampler resampler;
                resampler.resample(pcm_mono, mono_samples, resample_buf, resampled_count, 1);

                bm.send(Buffers::ALERT_RX_BUF, resample_buf, resampled_count * sizeof(int16_t), pdMS_TO_TICKS(100));
                samples_queued += resampled_count;
            }

            if (bytes_consumed > 0) {
                current_offset += bytes_consumed;
            } else {
                payload_len = 0;
            }
        }

        if (samples_queued == 0) {
            ESP_LOGW("AlertFileDecoder", "No audio decoded from %s", path);
        }
        return samples_queued > 0;
    }
};
