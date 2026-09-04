#pragma once

#include "IAudioDecoder.h"
#include "micro_opus/ogg_opus_decoder.h"

class OggOpusDecoderStrategy : public IAudioDecoder {
public:
    OggOpusDecoderStrategy();
    ~OggOpusDecoderStrategy() override = default;

    bool init(int targetSampleRate, int targetChannels) override;
    DecodeResult decode(const uint8_t* inData, size_t inLen,
                        int16_t* outPcm, size_t maxSamples,
                        size_t& bytesConsumed, size_t& samplesDecoded) override;
    void reset() override;
    const char* getName() const override { return "OggOpus"; }
    uint32_t getSourceSampleRate() const override;
    uint8_t getSourceChannels() const override;

private:
    micro_opus::OggOpusDecoder _decoder;
    int _targetSampleRate = 48000;
    int _targetChannels = 1;
};
