#include "RtpStreamer.h"
#include "app_types.h"
#include "common/AppLogger.h"
#include "esp_random.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include <climits>
#include <cstring>

RtpStreamer::RtpStreamer(const TxConfig &config, RingbufHandle_t tx_ring_buffer)
    : RtpTaskBase(config, tx_ring_buffer, "RtpStreamer"), m_tx_config(config) {}

void RtpStreamer::eventHandlerBridge(void *handler_arg, esp_event_base_t base,
                                     int32_t id, void *event_data) {
  RtpStreamer *instance = static_cast<RtpStreamer *>(handler_arg);
  if (instance == nullptr || instance->m_task_handle == nullptr)
    return;

  // AppEvent event_id = static_cast<AppEvent>(id);
  // xTaskNotify(instance->m_task_handle,
  //             (event_id == AppEvent::WAKE_WORD_DETECTED) ? 1 : 0,
  //             eSetValueWithOverwrite);
}

void RtpStreamer::processLoop() {
  m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (m_socket < 0) {
    LOGE_NET("Failed to create TX socket");
    return;
  }

  struct sockaddr_in dest_addr;
  dest_addr.sin_addr.s_addr = inet_addr(m_tx_config.target_ip.c_str());
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(m_tx_config.port);

  uint16_t seq_num = 0;
  uint32_t timestamp_tracker = 0;
  uint8_t tx_frame_buffer[12 + 1400];
  rtp_header_t *rtp_hdr = reinterpret_cast<rtp_header_t *>(tx_frame_buffer);

  rtp_hdr->version_p_x_cc = 0x80; // RTP Version 2
  rtp_hdr->payload_type = m_tx_config.payload_type;
  rtp_hdr->ssrc = lwip_htonl(esp_random());

  uint32_t notification_value = 1; // FORCED ACTIVE for testing
  bool was_enabled = false;
  int skip_packets = 0;

  while (m_is_running) {
    if (!m_is_enabled) {
      was_enabled = false;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (!was_enabled) {
      was_enabled = true;
      skip_packets = 8; // Discard first 8 packets (~160ms) of transient startup noise

      // Flush buffer on transition to clean out any old state
      size_t clear_size = 0;
      while (true) {
        uint8_t *stale = static_cast<uint8_t *>(
            xRingbufferReceive(m_ring_buffer, &clear_size, 0));
        if (!stale)
          break;
        vRingbufferReturnItem(m_ring_buffer, stale);
      }
      LOGI_NET("RTP Streamer active: flushed ringbuffer, ignoring first %d packets to suppress transient noise", skip_packets);
    }

    if (notification_value == 0) {
      xTaskNotifyWait(0x00, ULONG_MAX, &notification_value, pdMS_TO_TICKS(100));
      if (!m_is_running)
        break;

      if (notification_value == 0)
        continue;

      // Resuming... flush buffer
      size_t clear_size = 0;
      while (true) {
        uint8_t *stale = static_cast<uint8_t *>(
            xRingbufferReceive(m_ring_buffer, &clear_size, 0));
        if (!stale)
          break;
        vRingbufferReturnItem(m_ring_buffer, stale);
      }
      continue;
    }

    size_t chunk_size = 0;
    uint8_t *audio_ptr = static_cast<uint8_t *>(xRingbufferReceiveUpTo(
        m_ring_buffer, &chunk_size, pdMS_TO_TICKS(50), 1400));

    if (audio_ptr != nullptr) {
      if (skip_packets > 0) {
        skip_packets--;
        vRingbufferReturnItem(m_ring_buffer, audio_ptr);
        continue;
      }

      rtp_hdr->sequence_num = lwip_htons(seq_num++);
      rtp_hdr->timestamp = lwip_htonl(timestamp_tracker);

      if (m_tx_config.format == AudioStreamFormat::G711_ULAW) {
        timestamp_tracker += chunk_size;
      } else {
        timestamp_tracker += (chunk_size / 2);
      }

      std::memcpy(tx_frame_buffer + 12, audio_ptr, chunk_size);
      vRingbufferReturnItem(m_ring_buffer, audio_ptr);

      sendto(m_socket, tx_frame_buffer, 12 + chunk_size, 0,
             reinterpret_cast<struct sockaddr *>(&dest_addr),
             sizeof(dest_addr));
    }
  }

  LOGI_NET("RtpStreamer task exiting...");
  closeSocket();
}
