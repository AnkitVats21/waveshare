#include "RtpReceiver.h"
#include "common/AppLogger.h"
#include <string.h>
#include "lwip/sockets.h"

void RtpReceiver::processLoop() {
  if (m_socket < 0) {
    LOGE_NET("Invalid RX socket");
    return;
  }

  uint8_t rx_packet_buffer[1500];
  LOGI_NET("Smart RTP Listener Active on Port %d (Shared Socket)", m_config.port);

  while (m_is_running) {
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);

    int bytes_received = recvfrom(m_socket, rx_packet_buffer, sizeof(rx_packet_buffer), 0,
                                  (struct sockaddr *)&source_addr, &socklen);

    if (bytes_received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            continue; // Timeout, just check m_is_running again
        }
        LOGE_NET("recvfrom error: %d", errno);
        vTaskDelay(pdMS_TO_TICKS(100)); // Prevent tight loop on error
        continue;
    }

    if (bytes_received > 12 && m_is_running) {
      uint8_t *payload_ptr = rx_packet_buffer + 12;
      size_t payload_size = bytes_received - 12;

      BaseType_t ok = xRingbufferSend(m_ring_buffer, payload_ptr, payload_size, pdMS_TO_TICKS(10));
      if (ok != pdTRUE) {
        // Buffer full
      }
    }
  }

  LOGI_NET("RtpReceiver task exiting...");
  closeSocket();
}
