#pragma once

#include "core_sysdb/AudioRates.h"
#include "audio_core/AlertPlayer.h"
#include "audio_core/SpeakerPlayback.h"
#include "audio_core/Resampler.h"
#include "core_sysdb/BufferManager.h"
#include "app/media_player/AudioDecoderFactory.h"
#include "services/storage/StorageService.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include <cstdio>
#include <memory>

class AlertFileDecoder : public IAlertFileDecoder {
public:
    static AlertFileDecoder& getInstance() {
        static AlertFileDecoder instance;
        return instance;
    }

    bool playAlertFile(const char* path) override {
        if (!path || !Services::StorageService::getInstance().isMounted()) {
            return false;
        }
        if (!Services::StorageService::getInstance().fileExists(path)) {
            if (Services::StorageService::getInstance().fileExists("/sdcard/media/alert/alert.ogg")) {
                path = "/sdcard/media/alert/alert.ogg";
            } else {
                return false;
            }
        }

        FILE* f = fopen(path, "rb");
        if (!f) return false;

        ESP_LOGI("AlertFileDecoder", "Playing custom alert: %s", path);

        constexpr size_t READ_BUF_SIZE = 1024;
        uint8_t* read_buf = (uint8_t*)malloc(READ_BUF_SIZE);
        constexpr size_t MAX_SAMPLES = 4096;
        int16_t* pcm_buf = (int16_t*)heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        int16_t* resample_buf = (int16_t*)heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (!read_buf || !pcm_buf || !resample_buf) {
            ESP_LOGE("AlertFileDecoder", "Failed to allocate memory for alert decoding");
            if (read_buf) free(read_buf);
            if (pcm_buf) heap_caps_free(pcm_buf);
            if (resample_buf) heap_caps_free(resample_buf);
            fclose(f);
            return false;
        }

        std::unique_ptr<IAudioDecoder> decoder;
        size_t payload_len = 0;
        size_t current_offset = 0;
        auto& bm = BufferManager::getInstance();

        while (true) {
            if (payload_len == 0 || current_offset >= payload_len) {
                payload_len = fread(read_buf, 1, READ_BUF_SIZE, f);
                current_offset = 0;
                if (payload_len == 0) break; // EOF
            }

            if (!decoder) {
                // Temporary diagnostics: the factory has reported valid OggS files as
                // "ambiguous"; log exactly what the sniffer sees.
                const uint8_t* h = read_buf + current_offset;
                size_t hl = payload_len - current_offset;
                ESP_LOGI("AlertFileDecoder", "sniff: len=%u off=%u bytes=%02x %02x %02x %02x buf=%s ferror=%d int_free=%u",
                         (unsigned)hl, (unsigned)current_offset,
                         hl > 0 ? h[0] : 0, hl > 1 ? h[1] : 0, hl > 2 ? h[2] : 0, hl > 3 ? h[3] : 0,
                         esp_ptr_external_ram(read_buf) ? "psram" : "internal", ferror(f),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                decoder = AudioDecoderFactory::createDecoder(read_buf + current_offset, payload_len - current_offset);
                if (decoder) {
                    decoder->init(MIXER_SAMPLE_RATE, 1);
                } else {
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
            }

            if (bytes_consumed > 0) {
                current_offset += bytes_consumed;
            } else {
                payload_len = 0;
            }
        }

        free(read_buf);
        heap_caps_free(pcm_buf);
        heap_caps_free(resample_buf);
        fclose(f);
        return true;
    }
};
