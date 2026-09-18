#include "WebMOpusDecoder.h"

#include "opus.h"
#include "esp_log.h"
#include <cstring>
#include <algorithm>

static const char* TAG = "WebMOpus";

namespace {

// Checks if an EBML ID is a master element container that contains child elements
bool isMasterElement(uint64_t id) {
    switch (id) {
        case 0x1A45DFA3: // EBML Header
        case 0x18538067: // Segment
        case 0x1549A966: // Info
        case 0x1654AE6B: // Tracks
        case 0xAE:       // TrackEntry
        case 0x1F43B675: // Cluster
        case 0xA0:       // BlockGroup
            return true;
        default:
            return false;
    }
}

// Reads variable length integer (VINT)
bool readVint(const uint8_t* data, size_t size, size_t& offset, uint64_t& val, size_t& vintLen, bool preserveMask) {
    if (offset >= size) return false;
    uint8_t first = data[offset];
    if (first == 0) return false; // Invalid VINT

    int numBytes = 1;
    uint8_t mask = 0x80;
    while ((first & mask) == 0) {
        numBytes++;
        mask >>= 1;
    }

    if (offset + numBytes > size) return false; // Incomplete

    val = preserveMask ? first : (first & ~mask);
    for (int i = 1; i < numBytes; ++i) {
        val = (val << 8) | data[offset + i];
    }
    vintLen = numBytes;
    offset += numBytes;
    return true;
}

} // namespace

WebMOpusDecoder::WebMOpusDecoder() {
    _buffer.reserve(8192);
}

WebMOpusDecoder::~WebMOpusDecoder() {
    cleanupOpusDecoder();
}

bool WebMOpusDecoder::initOpusDecoder() {
    if (_opusDecoder) return true;

    int error = OPUS_OK;
    _opusDecoder = opus_decoder_create(48000, _channels, &error);
    if (error != OPUS_OK || !_opusDecoder) {
        ESP_LOGE(TAG, "Failed to create Opus decoder (err=%d)", error);
        _opusDecoder = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "Opus decoder initialized (48kHz, %d channels)", _channels);
    return true;
}

void WebMOpusDecoder::cleanupOpusDecoder() {
    if (_opusDecoder) {
        opus_decoder_destroy(_opusDecoder);
        _opusDecoder = nullptr;
    }
}

bool WebMOpusDecoder::init(int targetSampleRate, int targetChannels) {
    _targetSampleRate = targetSampleRate;
    _channels = (targetChannels > 0) ? targetChannels : 2;
    reset();
    return initOpusDecoder();
}

void WebMOpusDecoder::reset() {
    cleanupOpusDecoder();
    _buffer.clear();
    _skipRemaining = 0;
    _streamByteOffset = 0;
    _currentClusterOffset = 0xFFFFFFFF;
    _lastClusterTimeMs = 0;
    _samplesDecodedSinceCluster = 0;
    _timecodeScale = 1000000;
    initOpusDecoder();
}

void WebMOpusDecoder::setStreamByteOffset(uint32_t offset) {
    _streamByteOffset = offset;
    _currentClusterOffset = 0xFFFFFFFF;
    _samplesDecodedSinceCluster = 0;
}

uint32_t WebMOpusDecoder::getPositionMs() const {
    if (_channels == 0 || _targetSampleRate == 0) return _lastClusterTimeMs;
    uint64_t elapsedMs = (_samplesDecodedSinceCluster * 1000) / (_channels * _targetSampleRate);
    return _lastClusterTimeMs + static_cast<uint32_t>(elapsedMs);
}

bool WebMOpusDecoder::resyncToCluster(size_t& offset) {
    const uint8_t syncMarker[4] = { 0x1F, 0x43, 0xB6, 0x75 };
    if (_buffer.size() < 4) return false;
    for (size_t i = offset; i + 4 <= _buffer.size(); ++i) {
        if (memcmp(_buffer.data() + i, syncMarker, 4) == 0) {
            ESP_LOGI(TAG, "EBML resync successful: skipped %zu bytes to cluster", i - offset);
            offset = i;
            return true;
        }
    }
    return false;
}

DecodeResult WebMOpusDecoder::decode(const uint8_t* inData, size_t inLen,
                                     int16_t* outPcm, size_t maxSamples,
                                     size_t& bytesConsumed, size_t& samplesDecoded) {
    bytesConsumed = 0;
    samplesDecoded = 0;

    if (!initOpusDecoder()) {
        return DecodeResult::ERROR_DECODE_FAILED;
    }

    // Handle skip bytes remaining from previously skipped elements
    size_t inOffset = 0;
    if (_skipRemaining > 0 && inLen > 0) {
        size_t toSkip = std::min(_skipRemaining, inLen);
        _skipRemaining -= toSkip;
        inOffset += toSkip;
    }

    // Append incoming bytes to assembly buffer if room available (cap at 16KB)
    if (inOffset < inLen && _buffer.size() < 16384) {
        size_t toAppend = std::min(inLen - inOffset, static_cast<size_t>(16384 - _buffer.size()));
        _buffer.insert(_buffer.end(), inData + inOffset, inData + inOffset + toAppend);
        inOffset += toAppend;
        _streamByteOffset += toAppend;
    }
    bytesConsumed = inOffset;

    return processBuffer(outPcm, maxSamples, samplesDecoded);
}

DecodeResult WebMOpusDecoder::processBuffer(int16_t* outPcm, size_t maxSamples, size_t& samplesDecoded) {
    size_t offset = 0;

    while (offset < _buffer.size()) {
        // Handle any pending skip
        if (_skipRemaining > 0) {
            size_t available = _buffer.size() - offset;
            size_t toSkip = std::min(_skipRemaining, available);
            _skipRemaining -= toSkip;
            offset += toSkip;
            if (_skipRemaining > 0) {
                break;
            }
        }

        size_t elemStart = offset;
        uint64_t elemId = 0;
        size_t idLen = 0;
        if (!readVint(_buffer.data(), _buffer.size(), offset, elemId, idLen, true)) {
            // Check if buffer is getting large without header sync
            if (_buffer.size() - elemStart > 512) {
                if (resyncToCluster(offset)) {
                    continue;
                }
            }
            offset = elemStart;
            break;
        }

        uint64_t elemSize = 0;
        size_t sizeLen = 0;
        if (!readVint(_buffer.data(), _buffer.size(), offset, elemSize, sizeLen, false)) {
            offset = elemStart;
            break;
        }

        // Master element container: descend into it
        if (isMasterElement(elemId)) {
            if (elemId == 0x1F43B675) { // Cluster
                // Calculate absolute byte offset in the stream
                _currentClusterOffset = (_streamByteOffset - _buffer.size()) + elemStart;
            }
            continue;
        }

        // Timestamp / Timecode inside Cluster (ID = 0xE7)
        if (elemId == 0xE7) {
            if (_buffer.size() - offset < elemSize) {
                offset = elemStart;
                break;
            }
            uint64_t tc = 0;
            for (size_t i = 0; i < elemSize; ++i) {
                tc = (tc << 8) | _buffer[offset + i];
            }
            offset += elemSize;
            _lastClusterTimeMs = static_cast<uint32_t>((tc * _timecodeScale) / 1000000);
            _samplesDecodedSinceCluster = 0;

            if (_seekIndexCb && _currentClusterOffset != 0xFFFFFFFF) {
                _seekIndexCb(_lastClusterTimeMs, _currentClusterOffset);
                _currentClusterOffset = 0xFFFFFFFF;
            }
            continue;
        }

        // TimecodeScale inside Segment Info (ID = 0x2AD7B1)
        if (elemId == 0x2AD7B1) {
            if (_buffer.size() - offset < elemSize) {
                offset = elemStart;
                break;
            }
            uint64_t scale = 0;
            for (size_t i = 0; i < elemSize; ++i) {
                scale = (scale << 8) | _buffer[offset + i];
            }
            offset += elemSize;
            if (scale > 0) {
                _timecodeScale = scale;
            }
            continue;
        }

        // SimpleBlock: contains audio frames
        if (elemId == 0xA3) {
            if (_buffer.size() - offset < elemSize) {
                // Incomplete SimpleBlock in buffer; wait for more data
                offset = elemStart;
                break;
            }

            size_t blockDecoded = 0;
            DecodeResult res = decodeSimpleBlockPayload(
                _buffer.data() + offset, elemSize,
                outPcm + samplesDecoded, maxSamples - samplesDecoded,
                blockDecoded
            );

            offset += elemSize;
            samplesDecoded += blockDecoded;
            _samplesDecodedSinceCluster += blockDecoded;

            if (res != DecodeResult::OK && res != DecodeResult::NEED_MORE_DATA) {
                ESP_LOGW(TAG, "SimpleBlock decode warning: %d", (int)res);
            }

            if (samplesDecoded > 0) {
                // Yield immediately to consumer per Opus frame (20-40ms)
                break;
            }
        } else {
            // Unneeded element: skip its payload
            size_t available = _buffer.size() - offset;
            if (available >= elemSize) {
                offset += elemSize;
            } else {
                _skipRemaining = elemSize - available;
                offset = _buffer.size();
                break;
            }
        }
    }

    // Erase processed bytes from buffer
    if (offset > 0) {
        _buffer.erase(_buffer.begin(), _buffer.begin() + offset);
    }

    // Safety guard against runaway buffer size with resync attempt
    if (_buffer.size() > 65536) {
        ESP_LOGW(TAG, "Buffer exceeded 64KB without sync; attempting cluster resync");
        size_t resyncOffset = 0;
        if (resyncToCluster(resyncOffset)) {
            _buffer.erase(_buffer.begin(), _buffer.begin() + resyncOffset);
        } else {
            _buffer.clear();
            _skipRemaining = 0;
            return DecodeResult::ERROR_INVALID_STREAM;
        }
    }

    return (samplesDecoded > 0) ? DecodeResult::OK : DecodeResult::NEED_MORE_DATA;
}

DecodeResult WebMOpusDecoder::decodeSimpleBlockPayload(const uint8_t* payload, size_t payloadLen,
                                                       int16_t* outPcm, size_t maxSamples, size_t& samplesDecoded) {
    samplesDecoded = 0;
    if (payloadLen < 4) return DecodeResult::NEED_MORE_DATA;

    size_t pOffset = 0;
    uint64_t trackNum = 0;
    size_t trackLen = 0;
    if (!readVint(payload, payloadLen, pOffset, trackNum, trackLen, false)) {
        return DecodeResult::ERROR_INVALID_STREAM;
    }

    if (pOffset + 3 > payloadLen) return DecodeResult::NEED_MORE_DATA;
    pOffset += 2; // Skip 2-byte timecode
    uint8_t flags = payload[pOffset++];

    uint8_t lacing = (flags & 0x06) >> 1;
    const uint8_t* frameData = payload + pOffset;
    size_t frameLen = payloadLen - pOffset;

    if (lacing == 0) { // No lacing - single frame
        if (frameLen == 0) return DecodeResult::OK;

        int samplesPerChannel = opus_decode(
            _opusDecoder,
            frameData, frameLen,
            outPcm, maxSamples / _channels,
            0
        );

        if (samplesPerChannel > 0) {
            samplesDecoded = samplesPerChannel * _channels;
            return DecodeResult::OK;
        } else {
            ESP_LOGD(TAG, "opus_decode error: %d", samplesPerChannel);
            return DecodeResult::ERROR_DECODE_FAILED;
        }
    } else if (lacing == 1) { // Xiph lacing
        if (frameLen < 1) return DecodeResult::OK;
        uint8_t numFrames = frameData[0] + 1;
        size_t lOffset = 1;
        std::vector<size_t> sizes(numFrames, 0);

        for (uint8_t i = 0; i < numFrames - 1; ++i) {
            while (lOffset < frameLen) {
                uint8_t b = frameData[lOffset++];
                sizes[i] += b;
                if (b != 255) break;
            }
        }

        size_t totalLeading = 0;
        for (uint8_t i = 0; i < numFrames - 1; ++i) totalLeading += sizes[i];
        if (frameLen > lOffset + totalLeading) {
            sizes[numFrames - 1] = frameLen - lOffset - totalLeading;
        }

        size_t curOffset = lOffset;
        for (uint8_t i = 0; i < numFrames; ++i) {
            if (curOffset + sizes[i] > frameLen) break;
            if (maxSamples <= samplesDecoded) break;

            int samplesPerChannel = opus_decode(
                _opusDecoder,
                frameData + curOffset, sizes[i],
                outPcm + samplesDecoded, (maxSamples - samplesDecoded) / _channels,
                0
            );

            curOffset += sizes[i];
            if (samplesPerChannel > 0) {
                samplesDecoded += samplesPerChannel * _channels;
            }
        }
        return DecodeResult::OK;
    }

    return DecodeResult::OK;
}
