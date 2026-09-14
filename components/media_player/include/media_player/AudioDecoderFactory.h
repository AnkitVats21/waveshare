#pragma once

#include "IAudioDecoder.h"
#include <memory>
#include <cstdint>
#include <cstddef>

class AudioDecoderFactory {
public:
    /**
     * @brief Inspects header magic bytes and returns the matching IAudioDecoder strategy.
     * @param headerData Pointer to beginning of stream
     * @param len Available header length (at least 4 bytes recommended)
     * @return unique_ptr to configured IAudioDecoder implementation
     */
    static std::unique_ptr<IAudioDecoder> createDecoder(const uint8_t* headerData, size_t len);
};
