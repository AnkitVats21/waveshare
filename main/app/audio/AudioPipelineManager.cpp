#include "AudioPipelineManager.h"
#include "MicCapture.h"
#include "RtpReceiver.h"
#include "RtpStreamer.h"
#include "SpeakerPlayback.h"
#include "common/AppLogger.h"
#include "common/app_types.h"
#include "services/EventBus.h"

bool AudioPipelineManager::initialize(const GlobalSystemSettings &settings,
                                      const HardwareAudioHandles &hw_handles,
                                      GlobalPipelineContext &out_context) {

  LOGI_AUDIO(
      "Wi-Fi confirmed. Initializing Audio Streamers & Hardware Tasks...");

  // 1. Allocate Ring Buffers in PSRAM using safe BYTEBUF streaming mode
  out_context.tx_ring_buffer =
      xRingbufferCreateWithCaps(settings.buffer_size, RINGBUF_TYPE_BYTEBUF,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  out_context.rx_ring_buffer =
      xRingbufferCreateWithCaps(settings.buffer_size, RINGBUF_TYPE_BYTEBUF,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  // CRITICAL FIX: Ensure both allocations succeeded before configuring tasks
  if (out_context.tx_ring_buffer == NULL ||
      out_context.rx_ring_buffer == NULL) {
    LOGE_AUDIO("Fatal: Failed to allocate audio ring buffers in PSRAM!");

    // Clean up partial allocations to prevent memory leaks if one succeeded
    if (out_context.tx_ring_buffer)
      vRingbufferDelete(out_context.tx_ring_buffer);
    if (out_context.rx_ring_buffer)
      vRingbufferDelete(out_context.rx_ring_buffer);
    return false;
  }

  // 2. Start Hardware Tasks (Pinned to core securely)
  MicCaptureTask *mic_task = new MicCaptureTask();
  mic_task->start(settings, hw_handles.mic_rx_handle,
                  out_context.tx_ring_buffer);

  SpeakerPlaybackTask::start(settings, hw_handles.play_dev,
                             out_context.rx_ring_buffer);

  // 3. Configure RTP Transmitter
  RtpStreamer::TxConfig tx_cfg;
  tx_cfg.target_ip = settings.server_ip;
  tx_cfg.port = settings.tx_rtp_port;
  tx_cfg.format = settings.stream_format;
  tx_cfg.priority = settings.tx_priority;
  tx_cfg.stack_size = 4096;
  tx_cfg.core_id = settings.network_core_id;

  static RtpStreamer rtp_tx(tx_cfg, out_context.tx_ring_buffer);
  if (!rtp_tx.begin()) {
    LOGE_AUDIO("Failed to start RTP Transmitter");
    return false;
  }

  // 4. Configure RTP Receiver
  RtpTaskBase::CommonConfig rx_cfg;
  rx_cfg.port = settings.rx_rtp_port;
  rx_cfg.priority = settings.rx_priority;
  rx_cfg.stack_size = 4096;
  rx_cfg.core_id = settings.network_core_id;

  static RtpReceiver rtp_rx(rx_cfg, out_context.rx_ring_buffer, "rtp_receiver");
  if (!rtp_rx.begin()) {
    LOGE_AUDIO("Failed to start RTP Receiver");
    return false;
  }

  // 5. Register Callbacks to EventBus
  /*
  EventBus::getInstance().subscribe(APP_EVENTS, AppEvent::WAKE_WORD_DETECTED,
                                     &RtpStreamer::eventHandlerBridge, &rtp_tx);

  EventBus::getInstance().subscribe(APP_EVENTS, AppEvent::STOP_STREAMING,
                                     &RtpStreamer::eventHandlerBridge, &rtp_tx);
  */

  LOGI_AUDIO("Audio Pipeline Subsystems Deployment Successful!");
  return true;
}
