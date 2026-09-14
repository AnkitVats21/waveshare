#pragma once

#include <cstddef>
#include <cstdint>

// Shared linear-interpolation PCM resampling, used by every audio producer
// that needs to convert between sample rates (Voice upsample, Alert/Media
// decode, wake-word mic downsample, onboard-speaker downsample).
class IResampler {
public:
  virtual ~IResampler() = default;

  // src/dst are interleaved int16 PCM, `channels` samples per frame.
  virtual void resample(const int16_t* src, size_t srcFrames,
                         int16_t* dst, size_t dstFrames, int channels) const = 0;
};

class LinearResampler : public IResampler {
public:
  void resample(const int16_t* src, size_t srcFrames,
                int16_t* dst, size_t dstFrames, int channels) const override;
};

// Frame count that resampling srcFrames from srcRate to dstRate produces.
size_t computeResampledFrames(size_t srcFrames, uint32_t srcRate, uint32_t dstRate);
