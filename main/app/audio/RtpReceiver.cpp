#include "RtpReceiver.h"
#include "common/AppLogger.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

void RtpReceiver::processLoop() {
  m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (m_socket < 0) {
    LOGE_NET("Failed to create RX socket");
    vTaskDelete(NULL);
    return;
  }

  struct sockaddr_in bind_addr;
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons(m_config.port);

  if (bind(m_socket, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    LOGE_NET("Failed to bind RX socket on port %d", m_config.port);
    closeSocket();
    vTaskDelete(NULL);
    return;
  }

  uint8_t rx_packet_buffer[1500]; // Max Ethernet MTU
  LOGI_NET("Smart RTP Listener Active on Port %d", m_config.port);

  // uint16_t last_seq = 0;
  // bool first_packet = true;

  while (true) {
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);

    int bytes_received =
        recvfrom(m_socket, rx_packet_buffer, sizeof(rx_packet_buffer), 0,
                 (struct sockaddr *)&source_addr, &socklen);

    if (bytes_received > 12) {
      // 1. Parse RTP Header
      // uint16_t seq = (rx_packet_buffer[2] << 8) | rx_packet_buffer[3];

      // 2. Detect Packet Loss
      // if (!first_packet) {
      //     uint16_t expected = last_seq + 1;
      //     if (seq != expected) {
      //         LOGW_NET("RTP Gap Detected! Expected %d, Got %d", expected,
      //         seq);
      //     }
      // }
      // first_packet = false;
      // last_seq = seq;

      // 3. Extract Payload
      uint8_t *payload_ptr = rx_packet_buffer + 12;
      size_t payload_size = bytes_received - 12;

      // 4. Robust Transmit to RingBuffer
      // Using 50ms timeout to survive bursty network traffic
      BaseType_t ok = xRingbufferSend(m_ring_buffer, payload_ptr, payload_size,
                                      pdMS_TO_TICKS(50));

      if (ok != pdTRUE) {
        LOGW_NET("RX RingBuffer FULL - Packet dropped! Size: %zu",
                 payload_size);
      }
    }
  }
}

void RtpReceiver::closeSocket() {
  if (m_socket >= 0) {
    close(m_socket);
    m_socket = -1;
  }
}
