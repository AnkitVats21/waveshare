#include "AudioPipelineManager.h"
#include "MicCapture.h"
#include "RtpReceiver.h"
#include "RtpStreamer.h"
#include "SpeakerPlayback.h"
#include "common/AppLogger.h"

MicCaptureTask* AudioPipelineManager::m_mic_task = nullptr;
SpeakerPlaybackTask* AudioPipelineManager::m_speaker_task = nullptr;
RtpStreamer* AudioPipelineManager::m_rtp_tx = nullptr;
RtpReceiver* AudioPipelineManager::m_rtp_rx = nullptr;

bool AudioPipelineManager::initialize(const GlobalSystemSettings &settings,
                                      const HardwareAudioHandles &hw_handles,
                                      GlobalPipelineContext &out_context) {

  LOGI_AUDIO("Initializing Audio Pipeline (Sample Rate: %lu Hz)...", settings.sample_rate);

  // 1. Allocate Ring Buffers
  out_context.tx_ring_buffer = xRingbufferCreateWithCaps(settings.buffer_size, RINGBUF_TYPE_BYTEBUF,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  out_context.rx_ring_buffer = xRingbufferCreateWithCaps(settings.buffer_size, RINGBUF_TYPE_BYTEBUF,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (!out_context.tx_ring_buffer || !out_context.rx_ring_buffer) {
    LOGE_AUDIO("Failed to allocate audio ring buffers!");
    return false;
  }

  // 2. Start Hardware Tasks
  m_mic_task = new MicCaptureTask();
  m_mic_task->start(settings, hw_handles.mic_rx_handle, out_context.tx_ring_buffer);

  m_speaker_task = new SpeakerPlaybackTask();
  m_speaker_task->start(settings, hw_handles.play_dev, out_context.rx_ring_buffer);

  // 3. Start RTP Transmitter
  RtpStreamer::TxConfig tx_cfg;
  tx_cfg.target_ip = settings.server_ip;
  tx_cfg.port = settings.tx_rtp_port;
  tx_cfg.format = settings.stream_format;
  tx_cfg.priority = settings.tx_priority;
  tx_cfg.stack_size = 4096;
  tx_cfg.core_id = settings.network_core_id;

  m_rtp_tx = new RtpStreamer(tx_cfg, out_context.tx_ring_buffer);
  if (!m_rtp_tx->begin()) {
      LOGE_AUDIO("Failed to start RTP Streamer");
  }

  // 4. Start RTP Receiver
  RtpTaskBase::CommonConfig rx_cfg;
  rx_cfg.port = settings.rx_rtp_port;
  rx_cfg.priority = settings.rx_priority;
  rx_cfg.stack_size = 4096;
  rx_cfg.core_id = settings.network_core_id;

  m_rtp_rx = new RtpReceiver(rx_cfg, out_context.rx_ring_buffer, "rtp_receiver");
  if (!m_rtp_rx->begin()) {
      LOGE_AUDIO("Failed to start RTP Receiver");
  }

  return true;
}

void AudioPipelineManager::teardown(GlobalPipelineContext &context) {
    LOGI_AUDIO("Tearing down Audio Pipeline...");

    // 1. Stop high-level tasks first (RTP)
    if (m_rtp_tx) {
        m_rtp_tx->stop();
        delete m_rtp_tx;
        m_rtp_tx = nullptr;
    }
    if (m_rtp_rx) {
        m_rtp_rx->stop();
        delete m_rtp_rx;
        m_rtp_rx = nullptr;
    }

    // 2. Stop Hardware Tasks
    if (m_mic_task) {
        m_mic_task->stop();
        vTaskDelay(pdMS_TO_TICKS(50));
        delete m_mic_task;
        m_mic_task = nullptr;
    }

    if (m_speaker_task) {
        m_speaker_task->stop();
        vTaskDelay(pdMS_TO_TICKS(50));
        delete m_speaker_task;
        m_speaker_task = nullptr;
    }

    // 3. Cleanup Ring Buffers
    if (context.tx_ring_buffer) {
        vRingbufferDelete(context.tx_ring_buffer);
        context.tx_ring_buffer = nullptr;
    }
    if (context.rx_ring_buffer) {
        vRingbufferDelete(context.rx_ring_buffer);
        context.rx_ring_buffer = nullptr;
    }
    
    LOGI_AUDIO("Audio Pipeline teardown complete.");
}
