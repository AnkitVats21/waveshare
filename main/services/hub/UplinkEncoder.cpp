#include "services/hub/UplinkEncoder.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "opus.h"

namespace Services {

static const char* TAG = "UplinkEncoder";

UplinkEncoder::UplinkEncoder() = default;

UplinkEncoder::~UplinkEncoder() {
    if (m_enc) {
        heap_caps_free(m_enc);
        m_enc = nullptr;
    }
}

bool UplinkEncoder::init(int kbps, int complexity) {
    if (m_enc) {
        return reset(kbps, complexity);
    }

    int state_size = opus_encoder_get_size(1);
    if (state_size <= 0) {
        ESP_LOGE(TAG, "opus_encoder_get_size(1) failed: %d", state_size);
        return false;
    }

    void* state_mem = heap_caps_malloc(state_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!state_mem) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes for OpusEncoder in PSRAM", state_size);
        return false;
    }

    m_enc = reinterpret_cast<OpusEncoder*>(state_mem);
    // CELT-only: RESTRICTED_LOWDELAY avoids SILK double-precision float emulation on ESP32-S3
    int err = opus_encoder_init(m_enc, 16000, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY);
    if (err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_encoder_init failed: %s", opus_strerror(err));
        heap_caps_free(m_enc);
        m_enc = nullptr;
        return false;
    }

    m_kbps = kbps;
    m_complexity = complexity;
    opus_encoder_ctl(m_enc, OPUS_SET_BITRATE(kbps * 1000));
    opus_encoder_ctl(m_enc, OPUS_SET_COMPLEXITY(complexity));
    opus_encoder_ctl(m_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    ESP_LOGI(TAG, "Opus uplink encoder initialized (%d kbps, complexity %d, %d bytes in PSRAM)",
             kbps, complexity, state_size);
    return true;
}

bool UplinkEncoder::reset(int kbps, int complexity) {
    if (!m_enc) return false;
    opus_encoder_ctl(m_enc, OPUS_RESET_STATE);
    if (m_kbps != kbps) {
        m_kbps = kbps;
        opus_encoder_ctl(m_enc, OPUS_SET_BITRATE(kbps * 1000));
    }
    if (m_complexity != complexity) {
        m_complexity = complexity;
        opus_encoder_ctl(m_enc, OPUS_SET_COMPLEXITY(complexity));
    }
    return true;
}

int UplinkEncoder::encode(const int16_t* pcm_320, uint8_t* out_packet, size_t max_packet_bytes) {
    if (!m_enc || !pcm_320 || !out_packet || max_packet_bytes == 0) return OPUS_BAD_ARG;
    opus_int32 n = opus_encode(m_enc, pcm_320, static_cast<int>(FRAME_SAMPLES),
                               out_packet, static_cast<opus_int32>(max_packet_bytes));
    return static_cast<int>(n);
}

} // namespace Services
