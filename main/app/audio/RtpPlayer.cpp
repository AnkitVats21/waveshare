#include "RtpPlayer.h"
#include "SpeakerPlayback.h"
#include "common/AppLogger.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include <cstring>

namespace {
constexpr size_t kRtpHeaderBytes = 12;
constexpr uint16_t kExpectedPayloadType = 96;
constexpr uint16_t kMaxConcealedLossPackets = 3;
}

#pragma pack(push, 1)
struct rtp_header_t {
    uint8_t version_p_x_cc;
    uint8_t payload_type;
    uint16_t sequence_num;
    uint32_t timestamp;
    uint32_t ssrc;
};
#pragma pack(pop)

RtpPlayer::RtpPlayer()
    : ReactorTask({
          "rtp_player",
          ThreadConfig::StackSize::STACK_SMALL,
          ThreadConfig::Priority::NORMAL,
          ThreadConfig::CORE_NETWORK,
          COMP::SYSTEM | COMP::ASSISTANT
      })
{}

RtpPlayer::~RtpPlayer() {
    stopPlayer();
}

bool RtpPlayer::begin() {
    LOGI_SYSTEM("RtpPlayer operational.");
    return true;
}

void RtpPlayer::onStateChanged(ComponentMask changed, const SystemState& snap) {
    xTaskNotifyGive(m_task_handle);
}

void RtpPlayer::run() {
    LOGI_SYSTEM("RtpPlayer supervisor active.");

    while (m_running) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!m_running) break;

        auto snap = EmbeddedSysDb::getInstance().snapshot();
        bool wifi_ok = snap.system.wifi_connected;
        bool assistant_idle = (snap.assistant.session_state == AssistantState::Idle);

        if (wifi_ok && assistant_idle) {
            if (m_socket < 0) {
                startPlayer(snap);
            }
        } else {
            if (m_socket >= 0) {
                stopPlayer();
            }
        }
    }
}

void RtpPlayer::startPlayer(const SystemState& snap) {
    LOGI_SYSTEM("Starting RtpPlayer (WiFi OK, Assistant Idle).");
    m_rx_port = snap.audio.rtp_rx_port;
    m_seq_initialized = false;
    m_last_payload_bytes = 0;
    m_packets_received = 0;
    m_packets_concealed = 0;
    m_packets_dropped = 0;
    m_packets_reordered = 0;

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (m_socket < 0) {
        LOGE_SYSTEM("Failed to create RTP socket");
        return;
    }

    struct sockaddr_in bind_addr;
    std::memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(m_rx_port);

    if (bind(m_socket, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        LOGE_SYSTEM("Failed to bind RTP socket to port %d", m_rx_port);
        closeSocket();
        return;
    }

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    m_rx_running = true;
    xTaskCreatePinnedToCore(rxWorkerBridge, "rtp_rx", 6144, this, ThreadConfig::Priority::SPEAKER_PLAYBACK, &m_rx_task_handle, ThreadConfig::CORE_NETWORK);
}

void RtpPlayer::stopPlayer() {
    LOGW_SYSTEM("Stopping RtpPlayer.");
    m_rx_running = false;
    closeSocket();
    while (m_rx_task_handle != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    LOGI_SYSTEM("RTP stats: rx=%u concealed=%u dropped=%u reordered=%u",
                (unsigned)m_packets_received,
                (unsigned)m_packets_concealed,
                (unsigned)m_packets_dropped,
                (unsigned)m_packets_reordered);
    BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);
}

void RtpPlayer::rxWorkerBridge(void* arg) {
    static_cast<RtpPlayer*>(arg)->rxLoop();
}

void RtpPlayer::rxLoop() {
    struct sockaddr_in source_addr;
    socklen_t addr_len = sizeof(source_addr);

    while (m_rx_running) {
        int len = recvfrom(m_socket, m_rx_buffer.data(), m_rx_buffer.size(), 0,
                           (struct sockaddr*)&source_addr, &addr_len);
        if (len > static_cast<int>(kRtpHeaderBytes)) {
            const auto* hdr = reinterpret_cast<const rtp_header_t*>(m_rx_buffer.data());
            uint8_t version = hdr->version_p_x_cc >> 6;
            uint8_t payload_type = hdr->payload_type & 0x7F;
            if (version != 2 || payload_type != kExpectedPayloadType) {
                continue;
            }

            uint16_t seq = ntohs(hdr->sequence_num);
            size_t payload_bytes = static_cast<size_t>(len - kRtpHeaderBytes);

            auto snap = EmbeddedSysDb::getInstance().snapshot();
            if (snap.assistant.session_state == AssistantState::Idle) {
                if (m_seq_initialized) {
                    uint16_t expected_seq = static_cast<uint16_t>(m_last_seq + 1);
                    int16_t seq_delta = static_cast<int16_t>(seq - expected_seq);

                    if (seq_delta < 0) {
                        m_packets_reordered++;
                        continue;
                    }

                    if (seq_delta > 0) {
                        if (seq_delta <= kMaxConcealedLossPackets) {
                            size_t conceal_bytes = m_last_payload_bytes > 0 ? m_last_payload_bytes : payload_bytes;
                            if (conceal_bytes > m_conceal_buffer.size()) {
                                conceal_bytes = m_conceal_buffer.size();
                            }
                            for (int16_t i = 0; i < seq_delta; ++i) {
                                if (!BufferManager::getInstance().send(Buffers::SPK_RX_BUF, m_conceal_buffer.data(), conceal_bytes)) {
                                    m_packets_dropped++;
                                } else {
                                    m_packets_concealed++;
                                }
                            }
                        } else {
                            m_seq_initialized = false;
                        }
                    }
                }

                if (!BufferManager::getInstance().send(Buffers::SPK_RX_BUF, m_rx_buffer.data() + kRtpHeaderBytes, payload_bytes)) {
                    m_packets_dropped++;
                } else {
                    m_packets_received++;
                    m_last_seq = seq;
                    m_last_payload_bytes = payload_bytes;
                    m_seq_initialized = true;
                }
            }
        }
    }
    m_rx_task_handle = nullptr;
    vTaskDelete(NULL);
}

void RtpPlayer::closeSocket() {
    if (m_socket >= 0) {
        close(m_socket);
        m_socket = -1;
    }
}
