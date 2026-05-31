#pragma once

#include "common/system_settings.h"
#include "common/hw_types.h"

// Forward declarations
class MicCaptureTask;
class SpeakerPlaybackTask;
class RtpStreamer;
class RtpReceiver;

/**
 * @brief Manages the lifecycle of audio streaming and processing tasks.
 *
 * Ring buffers are owned by BufferManager (registered by MicCapture.h and
 * SpeakerPlayback.h at static-init time). AudioPipelineManager retrieves
 * handles from BufferManager — it no longer creates or destroys them.
 */
class AudioPipelineManager {
public:
    /**
     * @brief Initialize the audio pipeline (Tasks + RTP).
     * BufferManager::initAll() must have been called before this.
     */
    static bool initialize(const GlobalSystemSettings &settings,
                           const HardwareAudioHandles &hw_handles);

    /**
     * @brief Cleanly tear down the audio pipeline and free task resources.
     * Ring buffers are flushed but NOT deleted (BufferManager owns them).
     */
    static void teardown();

    /** @brief Toggle mic capture task + RTP streamer state. */
    static void setMicEnabled(bool enabled);

    /** @brief Gate just the RTP streamer without touching MicCapture reads. */
    static void setRtpEnabled(bool enabled);

    /** @brief Signal that the current turn is interrupted by a user barge-in. */
    static void setRtpRxInterrupted(bool interrupted);

private:
    static MicCaptureTask     *m_mic_task;
    static SpeakerPlaybackTask *m_speaker_task;
    static RtpStreamer         *m_rtp_tx;
    static RtpReceiver         *m_rtp_rx;
    static int                  m_shared_socket;
};
