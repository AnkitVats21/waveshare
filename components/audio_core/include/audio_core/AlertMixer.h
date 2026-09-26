#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

// Decoded alert audio: mono int16 PCM at MIXER_SAMPLE_RATE, held in PSRAM.
// A clip is never modified after it is handed to the mixer; replacing an
// alert swaps in a new clip, and the old one is freed when the last holder
// (the mixer, if it is playing) lets go of it.
class AlertClip {
public:
    // Longest clip kept; longer sources are cut and faded out.
    static constexpr uint32_t MAX_SECONDS = 3;

    static std::shared_ptr<AlertClip> create(size_t samples);
    ~AlertClip();

    int16_t* data() { return m_pcm; }
    const int16_t* data() const { return m_pcm; }
    size_t size() const { return m_samples; }
    // Trims the clip to its first `samples` samples (no reallocation).
    void shrink(size_t samples) { if (samples < m_samples) m_samples = samples; }
    // Linear fade-in over the first and fade-out over the last `samples`.
    void fadeEdges(size_t in_samples, size_t out_samples);

    AlertClip(const AlertClip&) = delete;
    AlertClip& operator=(const AlertClip&) = delete;

private:
    AlertClip(int16_t* pcm, size_t samples) : m_pcm(pcm), m_samples(samples) {}
    int16_t* m_pcm;
    size_t m_samples;
};

using AlertClipPtr = std::shared_ptr<const AlertClip>;

// Plays alert clips into the speaker mix. The speaker task pulls samples with
// render(); any task may call play() or stop().
//
// Only one alert sounds at a time and the latest request wins: a new alert
// fades the playing one out over FADE_SAMPLES and replaces it. Requests made
// while another is still pending replace the pending one, so a burst of state
// changes plays only the last chime instead of a backlog.
class AlertMixer {
public:
    static constexpr size_t SLOTS = 8;
    static constexpr size_t FADE_SAMPLES = 160;   // 5 ms at 32 kHz

    enum class Event : uint8_t { None, Started, Ended };

    void setClip(size_t slot, AlertClipPtr clip);
    AlertClipPtr clip(size_t slot) const;
    // Linear gain applied at playback; clamped to [0, 4].
    void setGain(size_t slot, float gain);
    float gain(size_t slot) const;
    void setEnabled(size_t slot, bool enabled);
    bool enabled(size_t slot) const;

    // Returns false if the slot is disabled or holds no clip.
    bool play(size_t slot);
    // Fades out whatever is playing and drops a pending request.
    void stop();

    // Writes up to n samples into out and returns how many were written; 0
    // when nothing is playing. `event` reports playback starting (idle to
    // playing) or ending (the last sample of the last alert was written).
    size_t render(int16_t* out, size_t n, Event& event);

    bool active() const;
    // Slot of the alert playing (or about to play), -1 when idle.
    int current() const;

private:
    static constexpr int NONE = -1;

    struct Slot {
        AlertClipPtr clip;
        int32_t gain_q12 = 4096;
        bool enabled = true;
    };

    void beginPending();

    mutable std::mutex m_mutex;
    Slot m_slots[SLOTS];

    // Playing voice
    AlertClipPtr m_clip;
    size_t m_pos = 0;
    int32_t m_gain_q12 = 4096;
    int m_slot = NONE;
    // Remaining samples of a fade-out; 0 when not fading
    size_t m_fade_left = 0;

    // Latest request, started once the playing clip has faded out
    int m_pending = NONE;
    bool m_stop_requested = false;
    bool m_was_active = false;
};
