#include "OggOpusDecoderStrategy.h"

#include <algorithm>
#include <cstring>

#include "esp_log.h"

static const char* TAG = "OggOpus";

namespace {
// The header pages must fit here; ours are about 100 bytes.
constexpr size_t MAX_HEAD_BYTES = 16 * 1024;
}

OggOpusDecoderStrategy::OggOpusDecoderStrategy() {}

bool OggOpusDecoderStrategy::init(int targetSampleRate, int targetChannels) {
    _targetSampleRate = targetSampleRate;
    _targetChannels = targetChannels;
    _decoder.reset();
    return true;
}

void OggOpusDecoderStrategy::setStreamHead(const uint8_t* head, size_t len) {
    if (!_haveHeader && head) _haveHeader = Media::parseOggHeader(head, len, _header);
}

void OggOpusDecoderStrategy::captureHead(const uint8_t* in, size_t len) {
    if (_haveHeader || _head.size() >= MAX_HEAD_BYTES) return;
    _head.insert(_head.end(), in, in + std::min(len, MAX_HEAD_BYTES - _head.size()));
    if (Media::parseOggHeader(_head.data(), _head.size(), _header)) {
        _haveHeader = true;
        std::vector<uint8_t>().swap(_head);
    }
}

void OggOpusDecoderStrategy::setSeekTarget(uint32_t positionMs) {
    if (!_haveHeader) {
        ESP_LOGW(TAG, "Seek to %u ms without the header pages: playing from where reading starts",
                 (unsigned)positionMs);
        return;
    }
    _scanner = std::make_unique<Media::OggSeekScanner>(_header.serial, _header.pre_skip, positionMs);
    _baseFrames = uint64_t(positionMs) * 48;
}

uint32_t OggOpusDecoderStrategy::getPositionMs() const {
    return static_cast<uint32_t>((_baseFrames + _outFrames) * 1000 / 48000);
}

DecodeResult OggOpusDecoderStrategy::feed(const uint8_t* in, size_t len, int16_t* outPcm, size_t maxSamples,
                                          size_t& consumed, size_t& samples) {
    micro_opus::OggOpusResult result =
        _decoder.decode(in, len, reinterpret_cast<uint8_t*>(outPcm), maxSamples * sizeof(int16_t), consumed, samples);
    if (result == micro_opus::OGG_OPUS_OUTPUT_BUFFER_TOO_SMALL) return DecodeResult::OUTPUT_BUFFER_FULL;
    if (result == micro_opus::OGG_OPUS_INPUT_INVALID) return DecodeResult::ERROR_INVALID_STREAM;
    if (result != micro_opus::OGG_OPUS_OK) return DecodeResult::ERROR_DECODE_FAILED;

    // After a seek: drop the frames before the target.
    if (_discardFrames > 0 && samples > 0) {
        const size_t ch = getSourceChannels();
        const size_t drop = std::min<size_t>(samples, _discardFrames);
        memmove(outPcm, outPcm + drop * ch, (samples - drop) * ch * sizeof(int16_t));
        samples -= drop;
        _discardFrames -= drop;
    }
    _outFrames += samples;
    return DecodeResult::OK;
}

DecodeResult OggOpusDecoderStrategy::decode(const uint8_t* inData, size_t inLen,
                                            int16_t* outPcm, size_t maxSamples,
                                            size_t& bytesConsumed, size_t& samplesDecoded) {
    bytesConsumed = 0;
    samplesDecoded = 0;

    if (_scanner) {
        if (!inData || inLen == 0) return DecodeResult::NEED_MORE_DATA;
        _scan.insert(_scan.end(), inData, inData + inLen);
        bytesConsumed = inLen;
        _scan.erase(_scan.begin(), _scan.begin() + _scanner->scan(_scan.data(), _scan.size()));
        if (!_scanner->found()) return DecodeResult::OK;
        _pending = Media::renumberHeader(_header, _scanner->startSeq());
        _pending.insert(_pending.end(), _scan.begin(), _scan.end());
        _pendingPos = 0;
        _discardFrames = _scanner->discardFrames();
        ESP_LOGI(TAG, "Seek: starting at page %u, dropping %u frames", (unsigned)_scanner->startSeq(),
                 (unsigned)_discardFrames);
        _scanner.reset();
        std::vector<uint8_t>().swap(_scan);
        return DecodeResult::OK;
    }

    DecodeResult r;
    if (_pendingPos < _pending.size()) {
        size_t used = 0;
        r = feed(_pending.data() + _pendingPos, _pending.size() - _pendingPos, outPcm, maxSamples, used,
                 samplesDecoded);
        _pendingPos += used;
        if (_pendingPos >= _pending.size() || r == DecodeResult::ERROR_INVALID_STREAM) {
            std::vector<uint8_t>().swap(_pending);
            _pendingPos = 0;
        }
    } else {
        if (!inData || inLen == 0) return DecodeResult::NEED_MORE_DATA;
        r = feed(inData, inLen, outPcm, maxSamples, bytesConsumed, samplesDecoded);
        captureHead(inData, bytesConsumed);
    }
    if (r != DecodeResult::OK) return r;
    if (samplesDecoded == 0 && bytesConsumed == 0 && _pending.empty()) return DecodeResult::NEED_MORE_DATA;
    samplesDecoded *= getSourceChannels();
    return DecodeResult::OK;
}

void OggOpusDecoderStrategy::reset() {
    _decoder.reset();
    _scanner.reset();
    std::vector<uint8_t>().swap(_scan);
    std::vector<uint8_t>().swap(_pending);
    _pendingPos = 0;
    _discardFrames = 0;
    _baseFrames = 0;
    _outFrames = 0;
}

uint32_t OggOpusDecoderStrategy::getSourceSampleRate() const {
    uint32_t rate = _decoder.get_sample_rate();
    return rate > 0 ? rate : 48000;
}

uint8_t OggOpusDecoderStrategy::getSourceChannels() const {
    uint8_t ch = _decoder.get_channels();
    return ch > 0 ? ch : 2;
}
