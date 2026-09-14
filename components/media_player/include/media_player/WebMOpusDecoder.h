#pragma once

#include "IAudioDecoder.h"
#include <vector>
#include <cstdint>

struct OpusDecoder;

class WebMOpusDecoder : public IAudioDecoder {
public:
    WebMOpusDecoder();
    ~WebMOpusDecoder() override;

    bool init(int targetSampleRate, int targetChannels) override;
    DecodeResult decode(const uint8_t* inData, size_t inLen,
                        int16_t* outPcm, size_t maxSamples,
                        size_t& bytesConsumed, size_t& samplesDecoded) override;
    void reset() override;
    const char* getName() const override { return "WebMOpus"; }
    uint32_t getSourceSampleRate() const override { return 48000; }
    uint8_t getSourceChannels() const override { return _channels; }

private:
    OpusDecoder* _opusDecoder = nullptr;
    int _targetSampleRate = 48000;
    uint8_t _channels = 2; // YouTube Opus streams are decoded at 48kHz stereo by default

    // Streaming assembly buffer for cross-chunk EBML elements
    std::vector<uint8_t> _buffer;
    size_t _skipRemaining = 0; // Bytes to skip from non-audio/unwanted elements

    bool initOpusDecoder();
    void cleanupOpusDecoder();
    
    // Parses and decodes one or more SimpleBlocks from _buffer
    DecodeResult processBuffer(int16_t* outPcm, size_t maxSamples, size_t& samplesDecoded);
    DecodeResult decodeSimpleBlockPayload(const uint8_t* payload, size_t payloadLen,
                                          int16_t* outPcm, size_t maxSamples, size_t& samplesDecoded);
};
