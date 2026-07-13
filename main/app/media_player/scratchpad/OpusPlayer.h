#pragma once

#include "common/TaskBase.h"
#include "services/BufferManager.h"
#include "micro_opus/ogg_opus_decoder.h"
#include "AudioSource.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>

// Declare the shared buffer slot. Size: 512 KB in PSRAM.
DECLARE_BUFFER(OPUS_COMP_BUF, "opus_comp", 512 * 1024)

class OpusPlayer : public TaskBase {
public:
    static OpusPlayer& getInstance();

    // Play using a configured audio source
    bool play(AudioSource* source);

    // Legacy file path play helper (internally configures and plays m_sd_source)
    bool play(const char* path);

    void stopPlayback();
    bool isPlaying() const { return m_playing.load(); }

protected:
    void run() override;

private:
    OpusPlayer();
    ~OpusPlayer() override;

    OpusPlayer(const OpusPlayer&) = delete;
    OpusPlayer& operator=(const OpusPlayer&) = delete;

    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_stopPlayback{false};

    AudioSource* m_source = nullptr;

    // Internal SD card source for legacy/compatibility path
    SdCardAudioSource m_sd_source;

    // Temporary PCM buffers
    int16_t* m_pcm_buffer = nullptr;
    size_t m_pcm_buffer_size = 0; // in bytes

    int16_t* m_resample_buffer = nullptr;
    size_t m_resample_buffer_samples = 0; // in samples

    micro_opus::OggOpusDecoder m_decoder;

    static constexpr const char* TAG = "OpusPlayer";
};