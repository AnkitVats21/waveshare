#include "OggOpusDecoderStrategy.h"

OggOpusDecoderStrategy::OggOpusDecoderStrategy() {}

bool OggOpusDecoderStrategy::init(int targetSampleRate, int targetChannels) {
    _targetSampleRate = targetSampleRate;
    _targetChannels = targetChannels;
    _decoder.reset();
    return true;
}

DecodeResult OggOpusDecoderStrategy::decode(const uint8_t* inData, size_t inLen,
                                            int16_t* outPcm, size_t maxSamples,
                                            size_t& bytesConsumed, size_t& samplesDecoded) {
    bytesConsumed = 0;
    samplesDecoded = 0;

    if (!inData || inLen == 0) {
        return DecodeResult::NEED_MORE_DATA;
    }

    size_t outByteCap = maxSamples * sizeof(int16_t);
    micro_opus::OggOpusResult result = _decoder.decode(
        inData, inLen,
        reinterpret_cast<uint8_t*>(outPcm), outByteCap,
        bytesConsumed, samplesDecoded
    );

    if (result == micro_opus::OGG_OPUS_OUTPUT_BUFFER_TOO_SMALL) {
        return DecodeResult::OUTPUT_BUFFER_FULL;
    } else if (result == micro_opus::OGG_OPUS_INPUT_INVALID) {
        return DecodeResult::ERROR_INVALID_STREAM;
    } else if (result != micro_opus::OGG_OPUS_OK) {
        return DecodeResult::ERROR_DECODE_FAILED;
    }

    if (samplesDecoded == 0 && bytesConsumed == 0) {
        return DecodeResult::NEED_MORE_DATA;
    }

    return DecodeResult::OK;
}

void OggOpusDecoderStrategy::reset() {
    _decoder.reset();
}

uint32_t OggOpusDecoderStrategy::getSourceSampleRate() const {
    uint32_t rate = _decoder.get_sample_rate();
    return rate > 0 ? rate : 48000;
}

uint8_t OggOpusDecoderStrategy::getSourceChannels() const {
    uint8_t ch = _decoder.get_channels();
    return ch > 0 ? ch : 2;
}
