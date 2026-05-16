#include "RtpReceiver.h"
#include "common/AppLogger.h"
#include <string.h>
#include "lwip/sockets.h"

void RtpReceiver::processLoop() {
  m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (m_socket < 0) {
    LOGE_NET("Failed to create RX socket");
    return;
  }

  struct sockaddr_in bind_addr;
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons(m_config.port);

  if (bind(m_socket, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    LOGE_NET("Failed to bind RX socket on port %d", m_config.port);
    closeSocket();
    return;
  }

  // Set receive timeout to prevent infinite blocking and allow m_is_running check
  struct timeval timeout;
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;
  setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  uint8_t rx_packet_buffer[1500];
  LOGI_NET("Smart RTP Listener Active on Port %d", m_config.port);

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
