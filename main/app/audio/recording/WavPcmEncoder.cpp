#include "app/audio/recording/WavPcmEncoder.h"
#include "services/storage/StorageService.h"
#include "esp_log.h"
#include <cstring>
#include <cstddef>

namespace {
static const char* TAG = "WavPcmEncoder";

#pragma pack(push, 1)
struct WavHeader {
    char     riff_id[4]      = {'R', 'I', 'F', 'F'};
    uint32_t riff_size       = 0; // patched at finalize: 36 + data_bytes
    char     wave_id[4]      = {'W', 'A', 'V', 'E'};
    char     fmt_id[4]       = {'f', 'm', 't', ' '};
    uint32_t fmt_size        = 16;
    uint16_t audio_format    = 1; // PCM
    uint16_t num_channels    = 0;
    uint32_t sample_rate     = 0;
    uint32_t byte_rate       = 0;
    uint16_t block_align     = 0;
    uint16_t bits_per_sample = 16;
    char     data_id[4]      = {'d', 'a', 't', 'a'};
    uint32_t data_size       = 0; // patched at finalize: total PCM byte count
};
#pragma pack(pop)
static_assert(sizeof(WavHeader) == 44, "WAV header must be exactly 44 bytes");
} // namespace

bool WavPcmEncoder::open(FILE* stream, uint32_t sample_rate, int channels, int bits_per_sample) {
    if (!stream) return false;
    m_stream          = stream;
    m_channels        = channels;
    m_bits_per_sample = bits_per_sample;
    m_bytes_written   = 0;

    WavHeader hdr;
    hdr.num_channels    = (uint16_t)channels;
    hdr.sample_rate      = sample_rate;
    hdr.bits_per_sample = (uint16_t)bits_per_sample;
    hdr.block_align      = (uint16_t)(channels * (bits_per_sample / 8));
    hdr.byte_rate         = sample_rate * hdr.block_align;

    size_t written = Services::StorageService::getInstance().writeStream(m_stream, &hdr, sizeof(hdr));
    if (written != sizeof(hdr)) {
        ESP_LOGE(TAG, "Failed to write WAV header (wrote %u/%u bytes)",
                 (unsigned)written, (unsigned)sizeof(hdr));
        return false;
    }
    return true;
}

bool WavPcmEncoder::writeSamples(const int16_t* data, size_t frame_count) {
    if (!m_stream || !data || frame_count == 0) return false;
    size_t bytes = frame_count * (size_t)m_channels * sizeof(int16_t);
    size_t written = Services::StorageService::getInstance().writeStream(m_stream, data, bytes);
    if (written != bytes) {
        ESP_LOGE(TAG, "Disk write error: expected %u bytes, wrote %u", (unsigned)bytes, (unsigned)written);
        return false;
    }
    m_bytes_written += (uint32_t)bytes;
    return true;
}

bool WavPcmEncoder::finalize() {
    if (!m_stream) return false;

    uint32_t riff_size = 36 + m_bytes_written;
    uint32_t data_size = m_bytes_written;

    fflush(m_stream);
    if (fseek(m_stream, offsetof(WavHeader, riff_size), SEEK_SET) != 0 ||
        fwrite(&riff_size, sizeof(riff_size), 1, m_stream) != 1) {
        ESP_LOGE(TAG, "Failed to patch riff_size in WAV header");
        return false;
    }
    if (fseek(m_stream, offsetof(WavHeader, data_size), SEEK_SET) != 0 ||
        fwrite(&data_size, sizeof(data_size), 1, m_stream) != 1) {
        ESP_LOGE(TAG, "Failed to patch data_size in WAV header");
        return false;
    }
    fflush(m_stream);
    ESP_LOGI(TAG, "finalize(): %u bytes of PCM written, header patched", (unsigned)m_bytes_written);
    return true;
}
