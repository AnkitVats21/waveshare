#pragma once

#include <memory>
#include <vector>

#include "IAudioDecoder.h"
#include "media_player/OggSeek.h"
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
    uint32_t getPositionMs() const override;
    // Seeks (Media::OggSeekScanner): the input then starts at an estimated
    // position before the target.
    void setSeekTarget(uint32_t positionMs) override;
    void setStreamHead(const uint8_t* head, size_t len) override;

private:
    DecodeResult feed(const uint8_t* in, size_t len, int16_t* outPcm, size_t maxSamples,
                      size_t& consumed, size_t& samples);
    void captureHead(const uint8_t* in, size_t len);

    micro_opus::OggOpusDecoder _decoder;
    int _targetSampleRate = 48000;
    int _targetChannels = 1;

    // The header pages, kept across resets for seeks; _head collects the
    // file's first bytes until they hold them.
    Media::OggHeader _header;
    bool _haveHeader = false;
    std::vector<uint8_t> _head;

    // Seeking: input collects in _scan until the scanner finds the page to
    // start at; then the renumbered header and that input are decoded from
    // _pending before new input.
    std::unique_ptr<Media::OggSeekScanner> _scanner;
    std::vector<uint8_t> _scan;
    std::vector<uint8_t> _pending;
    size_t _pendingPos = 0;
    uint32_t _discardFrames = 0;

    // Position: 48 kHz frames output since the start (or the seek target).
    uint64_t _baseFrames = 0;
    uint64_t _outFrames = 0;
};
