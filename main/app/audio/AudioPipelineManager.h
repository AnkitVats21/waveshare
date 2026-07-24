#pragma once

#include "common/hw_types.h"

// Forward declarations
class AudioHal;
class SpeakerPlaybackTask;
class BtSpeakerPlaybackTask;
class GeminiPCMDrainerTask;
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
    static bool initialize(uint32_t sample_rate,
                           AudioHal& hal,
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

    /** @brief Pause/resume the speaker playback task safely during clock switches. */
  static void pauseSpeaker();
  static void resumeSpeaker();

  /** @brief Pause/resume the speaker playback task draining (without pausing hardware). */
  static void setSpeakerPaused(bool paused);
  static bool isSpeakerPaused();

  /** @brief Suspend/resume the Gemini PCM drainer (used during alert playback). */
  static void suspendDrainer();
  static void resumeDrainer();

  /**
   * @brief Return the raw SpeakerPlaybackTask pointer so callers can poll
   *        isHardwarePaused() before reconfiguring the I2S clock.
   */
    static SpeakerPlaybackTask* getSpeakerTask() { return m_speaker_task; }

private:
    static SpeakerPlaybackTask*    m_speaker_task;
    static BtSpeakerPlaybackTask*  m_bt_speaker_task;
    static GeminiPCMDrainerTask*   m_drainer_task;
    static int                     m_shared_socket;
};
