#include "AudioPipelineManager.h"
#include "MicCapture.h"
#include "RtpReceiver.h"
#include "RtpStreamer.h"
#include "SpeakerPlayback.h"
#include "app/wake_word/WakeWordDetector.h"
#include "common/AppLogger.h"
#include "lwip/sockets.h"
#include <cstring>

MicCaptureTask* AudioPipelineManager::m_mic_task = nullptr;
SpeakerPlaybackTask* AudioPipelineManager::m_speaker_task = nullptr;
RtpStreamer* AudioPipelineManager::m_rtp_tx = nullptr;
RtpReceiver* AudioPipelineManager::m_rtp_rx = nullptr;
int AudioPipelineManager::m_shared_socket = -1;

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

  // If WakeWordDetector is running it is the SOLE hardware (I2S) reader.
  // Keep MicCapture soft-disabled so it never calls esp_codec_dev_read,
  // and instead route the streaming ring buffer through the detector's feedTask.
  if (WakeWordDetector::getInstance().isRunning()) {
    m_mic_task->setEnabled(false);
    WakeWordDetector::getInstance().setStreamRingbuf(out_context.tx_ring_buffer);
    LOGI_AUDIO("WakeWordDetector active: MicCapture disabled, streaming via feedTask");
  }

  m_speaker_task = new SpeakerPlaybackTask();
  m_speaker_task->start(settings, hw_handles.play_dev, out_context.rx_ring_buffer);

  // 3. Create Shared Bidirectional UDP Socket
  m_shared_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (m_shared_socket < 0) {
      LOGE_AUDIO("Failed to create shared RTP UDP socket!");
      return false;
  }

  struct sockaddr_in bind_addr;
  std::memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons(settings.rx_rtp_port); // Bind locally to the designated port (5005)

  if (bind(m_shared_socket, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
      LOGE_AUDIO("Failed to bind shared UDP socket on port %d!", settings.rx_rtp_port);
      close(m_shared_socket);
      m_shared_socket = -1;
      return false;
  }

  // Set receive timeout so that recvfrom doesn't block indefinitely on socket read
  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = 200000;
  if (setsockopt(m_shared_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
      LOGE_AUDIO("Failed to set receive timeout on shared UDP socket!");
  }

  // 4. Start RTP Transmitter (using shared socket)
  RtpStreamer::TxConfig tx_cfg;
  tx_cfg.target_ip = settings.server_ip;
  tx_cfg.port = settings.tx_rtp_port;
  tx_cfg.format = settings.stream_format;
  tx_cfg.priority = settings.tx_priority;
  tx_cfg.stack_size = 4096;
  tx_cfg.core_id = settings.network_core_id;

  m_rtp_tx = new RtpStreamer(tx_cfg, out_context.tx_ring_buffer, m_shared_socket);
  if (!m_rtp_tx->begin()) {
      LOGE_AUDIO("Failed to start RTP Streamer");
  }

  // 5. Start RTP Receiver (using shared socket)
  RtpTaskBase::CommonConfig rx_cfg;
  rx_cfg.port = settings.rx_rtp_port;
  rx_cfg.priority = settings.rx_priority;
  rx_cfg.stack_size = 4096;
  rx_cfg.core_id = settings.network_core_id;

  m_rtp_rx = new RtpReceiver(rx_cfg, out_context.rx_ring_buffer, m_shared_socket, "rtp_receiver");
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

    // 1b. Close the shared socket now that tasks are stopped
    if (m_shared_socket >= 0) {
        close(m_shared_socket);
        m_shared_socket = -1;
        LOGI_AUDIO("Shared UDP socket closed.");
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
    // Clear the detector's streaming reference first so feedTask stops writing
    WakeWordDetector::getInstance().setStreamRingbuf(nullptr);

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

void AudioPipelineManager::setMicEnabled(bool enabled) {
    if (m_mic_task) {
        m_mic_task->setEnabled(enabled);
    }
    if (m_rtp_tx) {
        m_rtp_tx->setEnabled(enabled);
    }
    LOGI_AUDIO("Mic pipeline %s (Task + RTP)", enabled ? "ENABLED" : "SOFT-DISABLED");
}

void AudioPipelineManager::setRtpEnabled(bool enabled) {
    if (m_rtp_tx) {
        m_rtp_tx->setEnabled(enabled);
    }
    LOGI_AUDIO("RTP streamer %s (WW mode)", enabled ? "ENABLED" : "DISABLED");
}

void AudioPipelineManager::setRtpRxInterrupted(bool interrupted) {
    if (m_rtp_rx) {
        m_rtp_rx->setInterrupted(interrupted);
    }
}
