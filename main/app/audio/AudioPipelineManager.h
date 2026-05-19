#pragma once

#include "common/app_types.h"

// Forward declarations
class MicCaptureTask;
class SpeakerPlaybackTask;
class RtpStreamer;
class RtpReceiver;

/**
 * @brief Manages the lifecycle of audio streaming and processing tasks
 */
class AudioPipelineManager {
public:
  /**
   * @brief Initialize the audio pipeline (Ring buffers, RTP, Hardware tasks)
   */
  static bool initialize(const GlobalSystemSettings &settings,
                         const HardwareAudioHandles &hw_handles,
                         GlobalPipelineContext &out_context);

  /**
   * @brief Cleanly tear down the audio pipeline and free resources
   */
  static void teardown(GlobalPipelineContext &context);

  /**
   * @brief Toggle mic capture task state
   */
  static void setMicEnabled(bool enabled);

  /** @brief Gate just the RTP streamer (network output) without touching
   *         MicCapture hardware reads. Used when WakeWordDetector owns I2S. */
  static void setRtpEnabled(bool enabled);

private:
  static MicCaptureTask *m_mic_task;
  static SpeakerPlaybackTask *m_speaker_task;
  static RtpStreamer *m_rtp_tx;
  static RtpReceiver *m_rtp_rx;
  static int m_shared_socket;
};
