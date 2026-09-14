#include "AudioDecoderFactory.h"
#include "WebMOpusDecoder.h"
#include "OggOpusDecoderStrategy.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "DecoderFactory";

std::unique_ptr<IAudioDecoder> AudioDecoderFactory::createDecoder(const uint8_t* headerData, size_t len) {
    if (headerData && len >= 4) {
        // Check for WebM/EBML magic: 0x1A 0x45 0xDF 0xA3
        if (headerData[0] == 0x1A && headerData[1] == 0x45 &&
            headerData[2] == 0xDF && headerData[3] == 0xA3) {
            ESP_LOGI(TAG, "Sniffed stream format: WebM (EBML)");
            return std::make_unique<WebMOpusDecoder>();
        }

        // Check for Ogg magic: 'O', 'g', 'g', 'S' (0x4F, 0x67, 0x67, 0x53)
        if (headerData[0] == 'O' && headerData[1] == 'g' &&
            headerData[2] == 'g' && headerData[3] == 'S') {
            ESP_LOGI(TAG, "Sniffed stream format: Ogg container");
            return std::make_unique<OggOpusDecoderStrategy>();
        }
    }

    ESP_LOGI(TAG, "Stream format ambiguous or uninitialized; defaulting to WebMOpus decoder");
    return std::make_unique<WebMOpusDecoder>();
}
