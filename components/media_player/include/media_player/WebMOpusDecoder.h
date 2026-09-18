#pragma once

#include "IAudioDecoder.h"
#include <vector>
#include <cstdint>
#include <functional>

struct OpusDecoder;

using SeekIndexCallback = std::function<void(uint32_t timecodeMs, uint32_t byteOffset)>;

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

    uint32_t getPositionMs() const override;
    void setStreamByteOffset(uint32_t offset) override;

    void setSeekIndexCallback(SeekIndexCallback cb) { _seekIndexCb = cb; }
    void clearSeekIndexCallback() { _seekIndexCb = nullptr; }

private:
    OpusDecoder* _opusDecoder = nullptr;
    int _targetSampleRate = 48000;
    uint8_t _channels = 2; // YouTube Opus streams are decoded at 48kHz stereo by default

    // Streaming assembly buffer for cross-chunk EBML elements
    std::vector<uint8_t> _buffer;
    size_t _skipRemaining = 0; // Bytes to skip from non-audio/unwanted elements

    // Stream byte position tracking (for accurate keyframe seek indexing)
    uint32_t _streamByteOffset = 0;
    uint32_t _currentClusterOffset = 0xFFFFFFFF;

    // Sub-cluster position interpolation
    uint32_t _lastClusterTimeMs = 0;
    uint64_t _samplesDecodedSinceCluster = 0;
    uint64_t _timecodeScale = 1000000; // 1ms default in WebM

    SeekIndexCallback _seekIndexCb = nullptr;

    bool initOpusDecoder();
    void cleanupOpusDecoder();
    
    // Scans buffer for next Cluster sync word (0x1F43B675)
    bool resyncToCluster(size_t& offset);

    // Parses and decodes one or more SimpleBlocks from _buffer
    DecodeResult processBuffer(int16_t* outPcm, size_t maxSamples, size_t& samplesDecoded);
    DecodeResult decodeSimpleBlockPayload(const uint8_t* payload, size_t payloadLen,
                                          int16_t* outPcm, size_t maxSamples, size_t& samplesDecoded);
};
