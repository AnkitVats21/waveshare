#pragma once

#include <cstddef>
#include <cstdint>

enum class DecodeResult {
    OK = 0,
    NEED_MORE_DATA,
    OUTPUT_BUFFER_FULL,
    ERROR_INVALID_STREAM,
    ERROR_DECODE_FAILED,
    STREAM_END
};

class IAudioDecoder {
public:
    virtual ~IAudioDecoder() = default;

    /**
     * @brief Initialize internal codec resources.
     */
    virtual bool init(int targetSampleRate, int targetChannels) = 0;

    /**
     * @brief Feed compressed container/audio chunk and decode to PCM.
     * @param inData Pointer to incoming compressed bytes
     * @param inLen Length of incoming bytes
     * @param outPcm Destination buffer for 16-bit PCM samples
     * @param maxSamples Capacity of outPcm in number of int16_t samples
     * @param bytesConsumed Number of bytes read from inData
     * @param samplesDecoded Number of int16_t samples written to outPcm
     */
    virtual DecodeResult decode(const uint8_t* inData, size_t inLen,
                                int16_t* outPcm, size_t maxSamples,
                                size_t& bytesConsumed, size_t& samplesDecoded) = 0;

    /**
     * @brief Reset decoder state between tracks.
     */
    virtual void reset() = 0;

    /**
     * @brief Codec / format identifier name.
     */
    virtual const char* getName() const = 0;

    /**
     * @brief Native sample rate reported by decoded stream.
     */
    virtual uint32_t getSourceSampleRate() const = 0;

    /**
     * @brief Native channel count reported by decoded stream.
     */
    virtual uint8_t getSourceChannels() const = 0;
};
