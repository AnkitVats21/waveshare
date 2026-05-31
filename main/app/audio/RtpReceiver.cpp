// #include "RtpReceiver.h"
// #include "common/AppLogger.h"
// #include <string.h>
// #include "lwip/sockets.h"
// #include "app/event/EventBus.h"
// #include "common/app_types.h"

// void RtpReceiver::processLoop() {
//   if (m_socket < 0) {
//     LOGE_NET("Invalid RX socket");
//     return;
//   }

//   m_is_interrupted = false; // Explicitly ensure we start in non-interrupted state!

//   uint8_t rx_packet_buffer[1500];
//   LOGI_NET("Smart RTP Listener Active on Port %d (Shared Socket)", m_config.port);

//   bool is_talking = false;
//   TickType_t last_rtp_packet_time = 0;

//   while (m_is_running) {
//     struct sockaddr_in source_addr;
//     socklen_t socklen = sizeof(source_addr);

//     int bytes_received = recvfrom(m_socket, rx_packet_buffer, sizeof(rx_packet_buffer), 0,
//                                   (struct sockaddr *)&source_addr, &socklen);

//     if (bytes_received < 0) {
//         if (errno == EAGAIN || errno == EWOULDBLOCK) {
//             // Check for silence timeout if we were previously talking
//             if (is_talking && (xTaskGetTickCount() - last_rtp_packet_time) > pdMS_TO_TICKS(800)) {
//                 is_talking = false;
//                 if (!m_is_interrupted) {
//                     LOGI_NET("RTP silence detected: publishing ASSISTANT_SILENT");
//                     EventBus::getInstance().publish(APP_EVENTS, AppEvent::ASSISTANT_SILENT, 0);
//                 }
//             }
//             continue; // Timeout, check m_is_running again
//         }
//         LOGE_NET("recvfrom error: %d", errno);
//         vTaskDelay(pdMS_TO_TICKS(100)); // Prevent tight loop on error
//         continue;
//     }

//     if (bytes_received > 0 && m_is_running) {
//       // 1. Is this a control packet or an RTP packet?
//       bool is_rtp = (bytes_received >= 12) && ((rx_packet_buffer[0] >> 6) == 2);

//       if (!is_rtp) {
//         // Control message received
//         uint8_t msg_type = rx_packet_buffer[0];
//         LOGI_NET("Control packet received: 0x%02X", msg_type);
//         if (msg_type == 0x05) { // MsgAssistantStop
//           LOGI_NET("Go Server sent MsgAssistantStop: publishing ASSISTANT_TURN_COMPLETE");
//           m_is_interrupted = false; // Reset interrupted flag
//           EventBus::getInstance().publish(APP_EVENTS, AppEvent::ASSISTANT_TURN_COMPLETE, 0);
//           is_talking = false;
//         } else if (msg_type == 0x06) { // MsgBacklightOn / talking start
//           if (!m_is_interrupted) {
//             LOGI_NET("Go Server sent MsgBacklightOn: publishing ASSISTANT_TALKING");
//             EventBus::getInstance().publish(APP_EVENTS, AppEvent::ASSISTANT_TALKING, 0);
//             is_talking = true;
//             last_rtp_packet_time = xTaskGetTickCount();
//           }
//         } else if (msg_type == 0x07) { // MsgBacklightOff / talking stop (silent)
//           if (!m_is_interrupted) {
//             LOGI_NET("Go Server sent MsgBacklightOff: publishing ASSISTANT_SILENT");
//             EventBus::getInstance().publish(APP_EVENTS, AppEvent::ASSISTANT_SILENT, 0);
//             is_talking = false;
//           }
//         }
//       } else {
//         // RTP Audio Packet received
//         if (m_is_interrupted) {
//           continue; // Discard trailing RTP packets while in interrupted state
//         }

//         uint8_t *payload_ptr = rx_packet_buffer + 12;
//         size_t payload_size = bytes_received - 12;
//         // TODO: if the packet is music plyback packet or AI assistant plyback packet

//         if (!is_talking) {
//           is_talking = true;
//           LOGI_NET("RTP stream active: publishing ASSISTANT_TALKING");
//           EventBus::getInstance().publish(APP_EVENTS, AppEvent::ASSISTANT_TALKING, 0);
//         }
//         last_rtp_packet_time = xTaskGetTickCount();

//         BaseType_t ok = xRingbufferSend(m_ring_buffer, payload_ptr, payload_size, pdMS_TO_TICKS(10));
//         if (ok != pdTRUE) {
//           // Buffer full (silent fail/ignore)
//         }
//       }
//     }
//   }

//   LOGI_NET("RtpReceiver task exiting...");
//   closeSocket();
// }




#include "RtpReceiver.h"
#include "common/AppLogger.h"
#include <string.h>
#include "lwip/sockets.h"
#include "app/event/EventBus.h"
#include "common/app_types.h"
#include "services/BufferManager.h"

// Define your SSRC Constants (Match these with your Go Server configuration)
#define SSRC_AI_ASSISTANT  0x11223344  // Example SSRC for AI voice
#define SSRC_MUSIC_PLAYBACK 0x55667788 // Example SSRC for Music

void RtpReceiver::processLoop() {
  if (m_socket < 0) {
    LOGE_NET("Invalid RX socket");
    return;
  }

  m_is_interrupted = false; 

  uint8_t rx_packet_buffer[1500];
  LOGI_NET("Smart RTP Listener Active on Port %d (Shared Socket)", m_config.port);

  bool is_talking = false;
  TickType_t last_rtp_packet_time = 0;

  while (m_is_running) {
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);

    int bytes_received = recvfrom(m_socket, rx_packet_buffer, sizeof(rx_packet_buffer), 0,
                                  (struct sockaddr *)&source_addr, &socklen);

    if (bytes_received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (is_talking && (xTaskGetTickCount() - last_rtp_packet_time) > pdMS_TO_TICKS(800)) {
                is_talking = false;
                if (!m_is_interrupted) {
                    LOGI_NET("RTP silence detected: publishing ASSISTANT_SILENT");
                    EventBus::getInstance().publish(APP_EVENTS, AppEvent::ASSISTANT_SILENT, 0);
                }
            }
            continue; 
        }
        LOGE_NET("recvfrom error: %d", errno);
        vTaskDelay(pdMS_TO_TICKS(100)); 
        continue;
    }

    if (bytes_received > 0 && m_is_running) {
      bool is_rtp = (bytes_received >= 12) && ((rx_packet_buffer[0] >> 6) == 2);

      if (!is_rtp) {
        // [Control Packet Logic Remains Unchanged]
        uint8_t msg_type = rx_packet_buffer[0];
        LOGI_NET("Control packet received: 0x%02X", msg_type);
        if (msg_type == 0x05) { 
          LOGI_NET("Go Server sent MsgAssistantStop: publishing ASSISTANT_TURN_COMPLETE");
          m_is_interrupted = false; 
          EventBus::getInstance().publish(APP_EVENTS, AppEvent::ASSISTANT_TURN_COMPLETE, 0);
          is_talking = false;
        } else if (msg_type == 0x06) { 
          if (!m_is_interrupted) {
            LOGI_NET("Go Server sent MsgBacklightOn: publishing ASSISTANT_TALKING");
            EventBus::getInstance().publish(APP_EVENTS, AppEvent::ASSISTANT_TALKING, 0);
            is_talking = true;
            last_rtp_packet_time = xTaskGetTickCount();
          }
        } else if (msg_type == 0x07) { 
          if (!m_is_interrupted) {
            LOGI_NET("Go Server sent MsgBacklightOff: publishing ASSISTANT_SILENT");
            EventBus::getInstance().publish(APP_EVENTS, AppEvent::ASSISTANT_SILENT, 0);
            is_talking = false;
          }
        }
      } else {
        // ── RTP Audio Packet Received ─────────────────────────────────────────
        
        // Extract 32-bit SSRC from bytes 8, 9, 10, 11 (Big Endian to Host conversion)
        uint32_t packet_ssrc = 0;
        memcpy(&packet_ssrc, &rx_packet_buffer[8], 4);
        packet_ssrc = ntohl(packet_ssrc);

        uint8_t *payload_ptr = rx_packet_buffer + 12;
        size_t payload_size = bytes_received - 12;

        // Route packet based on extracted SSRC stream type
        if (packet_ssrc == SSRC_AI_ASSISTANT) {
          
          if (m_is_interrupted) {
            continue; // Discard trailing AI packets if user hit barge-in / interrupted
          }

          if (!is_talking) {
            is_talking = true;
            LOGI_NET("AI Assistant RTP stream active: publishing ASSISTANT_TALKING");
            EventBus::getInstance().publish(APP_EVENTS, AppEvent::ASSISTANT_TALKING, 0);
          }
          last_rtp_packet_time = xTaskGetTickCount();

          // Stream to speaker ring buffer
          BufferManager::getInstance().send(m_buf_id, payload_ptr, payload_size,
                                            pdMS_TO_TICKS(10));

        } 
        else if (packet_ssrc == SSRC_MUSIC_PLAYBACK) {
          // ── Music Pipeline (Bypasses State Machine Events) ───────────────────
          
          // NOTE: WakeNet stays fully active! The user can still call out the wake-word
          // to interrupt the music playback seamlessly.
          BufferManager::getInstance().send(m_buf_id, payload_ptr, payload_size,
                                            pdMS_TO_TICKS(10));
        } 
        else {
          LOGW_NET("Unknown RTP SSRC encountered: 0x%08X", packet_ssrc);
        }
      }
    }
  }

  LOGI_NET("RtpReceiver task exiting...");
  closeSocket();
}
