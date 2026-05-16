# ESP32-S3 Audio Pipeline Mono Optimization Refactor

## Objective

Refactor the RTP playback pipeline to use a fully mono audio path instead of converting mono RTP audio into stereo internally.

This change is expected to significantly reduce:

- RTP packet drops
- Ringbuffer overflow
- Audio glitches
- DMA congestion
- CPU usage
- Memory bandwidth usage
- Playback latency

Target platform:

- ESP32-S3
- ES8311 codec
- Single physical speaker

---

# Root Cause Summary

Current architecture:

```text
Mono RTP Stream
    ↓
Software Mono → Stereo Expansion
    ↓
Stereo I2S DMA
    ↓
Stereo Codec Write
    ↓
Single Physical Speaker
```

This unnecessarily doubles:

- audio memory traffic
- DMA bandwidth
- codec throughput
- memcpy operations
- I2S traffic

The system logs confirm playback cannot consume audio fast enough:

```text
RX RingBuffer FULL - Packet dropped!
RTP Gap Detected!
```

This indicates:

```text
Playback pipeline throughput < incoming RTP throughput
```

---

# Required Refactor

The pipeline should remain MONO end-to-end.

New architecture:

```text
Mono RTP Stream
    ↓
Mono Ringbuffer
    ↓
Mono I2S
    ↓
Mono Codec Write
    ↓
Speaker
```

---

# Part 1 — Update I2S Configuration

## File

```text
Board.cpp
```

## Existing Code

```cpp
I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
    I2S_DATA_BIT_WIDTH_16BIT,
    I2S_SLOT_MODE_STEREO)
```

## Required Change

Replace:

```cpp
I2S_SLOT_MODE_STEREO
```

with:

```cpp
I2S_SLOT_MODE_MONO
```

---

# Part 2 — Update Codec Channel Count

## File

```text
Board.cpp
```

## Existing Code

```cpp
fs.channel = 2;
```

## Required Change

Replace with:

```cpp
fs.channel = 1;
```

This ensures the codec pipeline is configured for mono audio.

---

# Part 3 — Remove Stereo Expansion Logic

## File

```text
SpeakerPlayback.cpp
```

---

## Remove Stereo Buffer Allocation

Delete:

```cpp
const size_t MAX_SAMPLES = 1024;
int16_t *stereo_buffer =
    (int16_t *)malloc(MAX_SAMPLES * 2 * sizeof(int16_t));
```

---

## Remove Stereo Duplication Loop

Delete:

```cpp
for (size_t i = 0; i < num_samples && i < MAX_SAMPLES; i++) {
    stereo_buffer[i * 2] = pcm_ptr[i];
    stereo_buffer[i * 2 + 1] = pcm_ptr[i];
}
```

This mono-to-stereo expansion is no longer needed.

---

# Part 4 — Write PCM Directly

## Existing Code

```cpp
esp_codec_dev_write(
    param->device,
    stereo_buffer,
    num_samples * 2 * sizeof(int16_t));
```

## Required Change

Replace with:

```cpp
esp_codec_dev_write(
    param->device,
    pcm_ptr,
    rx_chunk_bytes);
```

This removes:

- extra memcpy
- extra DMA traffic
- doubled playback bandwidth
- unnecessary buffer processing

---

# Part 5 — Add Codec Write Timing Logs

## Objective

Measure whether playback writes are blocking excessively.

## Add Timing Instrumentation

Before:

```cpp
esp_codec_dev_write(...)
```

Add:

```cpp
uint64_t start = esp_timer_get_time();
```

After:

```cpp
uint64_t end = esp_timer_get_time();

LOGI_HAL(
    "Codec write took %llu us",
    end - start);
```

This helps validate whether playback is lagging behind realtime.

---

# Part 6 — Update RTP Packet Duration

## Current Sender Configuration

```python
CHUNK_SAMPLES = 320
```

At:

```text
32000 Hz
```

This equals:

```text
10ms RTP packets
```

Calculation:

320 / 32000 = 0.01 seconds

The comment claiming 20ms is incorrect.

---

# Recommended Change

Use:

```python
CHUNK_SAMPLES = 640
```

This creates true 20ms RTP packets at 32kHz.

Benefits:

- fewer UDP packets/sec
- lower scheduler pressure
- lower WiFi interrupt rate
- reduced socket overhead
- smoother buffering

---

# Part 7 — Recommended DMA Tuning

## Existing Configuration

```cpp
dma_desc_num = 8;
dma_frame_num = 240;
```

## Recommended Configuration

```cpp
dma_desc_num = 12;
dma_frame_num = 512;
```

This improves DMA resilience under WiFi bursts.

---

# Part 8 — Ringbuffer Validation

Keep ringbuffer overflow logging enabled.

Expected improvement after mono conversion:

- ringbuffer occupancy stabilizes
- no overflow warnings
- no RTP gap warnings

---

# Expected Outcome

After refactor:

## Expected Improvements

- significantly lower CPU usage
- lower memory bandwidth consumption
- smoother playback
- reduced packet loss
- reduced playback latency
- stable realtime audio

---

# Important Notes

## Mono Physical Speaker

Even though the board has a single speaker, the codec may internally support stereo paths.

However:

- software stereo duplication is unnecessary
- mono I2S mode is preferable
- mono codec configuration is preferable

---

# Validation Checklist

After implementation verify:

- no RX ringbuffer overflow
- no RTP gap warnings
- stable codec write timing
- stable long-duration playback
- lower CPU usage
- smoother playback quality

---

# Future Improvements (Optional)

Once mono optimization is stable:

## Add RTP Jitter Buffer

Architecture:

```text
UDP RX
    ↓
RTP Parser
    ↓
Jitter Buffer
    ↓
Playback Scheduler
    ↓
PCM Queue
    ↓
I2S
```

## Add RTP Timestamp Pacing

This prevents long-term playback drift.

---

# Final Goal

Target stable configuration:

```text
ESP32-S3
16-bit PCM
Mono
32kHz or 16kHz
20ms RTP packets
Stable realtime playback
No audio glitches
```
