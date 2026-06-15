#include "RtpTransceiver.h"
#include "MicCapture.h"
#include "SpeakerPlayback.h"
#include "common/AppLogger.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "esp_random.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include <cstring>

#pragma pack(push, 1)
struct rtp_header_t {
    uint8_t version_p_x_cc;
    uint8_t payload_type;
    uint16_t sequence_num;
    uint32_t timestamp;
    uint32_t ssrc;
};
#pragma pack(pop)

RtpTransceiver::RtpTransceiver()
    : ReactorTask({
          "rtp_trans",
          ThreadConfig::StackSize::STACK_SMALL,
          ThreadConfig::Priority::NORMAL,
          ThreadConfig::CORE_NETWORK,
          COMP::SYSTEM | COMP::PIPELINE
      })
{}

RtpTransceiver::~RtpTransceiver() {
    stopTransceiver();
}

bool RtpTransceiver::begin() {
    LOGI_SYSTEM("RtpTransceiver operational.");
    return true;
}

void RtpTransceiver::onStateChanged(ComponentMask changed, const SystemState& snap) {
    xTaskNotifyGive(m_task_handle);
}

void RtpTransceiver::run() {
    LOGI_SYSTEM("RtpTransceiver supervisor active.");

    while (m_running) {
        auto snap = EmbeddedSysDb::getInstance().snapshot();
        bool tx_active = (m_socket >= 0 && snap.pipeline.rtp_tx_en);
        TickType_t wait_ticks = tx_active ? 0 : portMAX_DELAY;

        ulTaskNotifyTake(pdTRUE, wait_ticks);
        if (!m_running) break;

        // Take a fresh snapshot to check states
        snap = EmbeddedSysDb::getInstance().snapshot();
        bool wifi_ok = snap.system.wifi_connected;
        bool mode_ok = (snap.pipeline.mode == PipelineMode::RTP_REMOTE || 
                        snap.pipeline.mode == PipelineMode::RTP_WAKEWORD);
        bool enabled = snap.pipeline.rtp_enabled;

        if (wifi_ok && mode_ok && enabled) {
            if (m_socket < 0) {
                startTransceiver(snap);
            }
        } else {
            if (m_socket >= 0) {
                stopTransceiver();
            }
        }

        // Transmitter logic (if active)
        if (m_socket >= 0 && snap.pipeline.rtp_tx_en) {
            size_t chunk_size = 0;
            // Block on receive with a 20ms timeout
            uint8_t *audio_ptr = static_cast<uint8_t *>(
                BufferManager::getInstance().receive(Buffers::RTP_MIC_BUF, &chunk_size, pdMS_TO_TICKS(20), 1400));

            if (audio_ptr) {
                static uint16_t seq_num = 0;
                static uint32_t timestamp = 0;
                
                struct sockaddr_in dest_addr;
                dest_addr.sin_addr.s_addr = inet_addr(m_target_ip.c_str());
                dest_addr.sin_family = AF_INET;
                dest_addr.sin_port = htons(m_tx_port);

                uint8_t packet[12 + 1400];
                rtp_header_t* hdr = (rtp_header_t*)packet;
                hdr->version_p_x_cc = 0x80;
                hdr->payload_type = (uint8_t)snap.audio.stream_format; 
                hdr->sequence_num = lwip_htons(seq_num++);
                hdr->timestamp = lwip_htonl(timestamp);
                hdr->ssrc = 0; 

                if (snap.audio.stream_format == AudioStreamFormat::G711_ULAW) {
                    timestamp += chunk_size;
                } else {
                    timestamp += (chunk_size / 2);
                }

                std::memcpy(packet + 12, audio_ptr, chunk_size);
                sendto(m_socket, packet, 12 + chunk_size, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
                
                BufferManager::getInstance().returnItem(Buffers::RTP_MIC_BUF, audio_ptr);
            }
        }
    }
}

void RtpTransceiver::startTransceiver(const SystemState& snap) {
    LOGI_SYSTEM("Starting RtpTransceiver (WiFi OK, Mode active).");
    m_target_ip = snap.system.server_ip;
    m_tx_port = snap.audio.rtp_tx_port;
    m_rx_port = snap.audio.rtp_rx_port;

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
    xTaskCreatePinnedToCore(rxWorkerBridge, "rtp_rx", 4096, this, ThreadConfig::Priority::SPEAKER_PLAYBACK, &m_rx_task_handle, ThreadConfig::CORE_NETWORK);
}

void RtpTransceiver::stopTransceiver() {
    LOGW_SYSTEM("Stopping RtpTransceiver.");
    m_rx_running = false;
    closeSocket();
    while (m_rx_task_handle != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    BufferManager::getInstance().flush(Buffers::RTP_MIC_BUF);
    BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);
}

void RtpTransceiver::rxWorkerBridge(void* arg) {
    static_cast<RtpTransceiver*>(arg)->rxLoop();
}

void RtpTransceiver::rxLoop() {
    uint8_t buffer[1500];
    struct sockaddr_in source_addr;
    socklen_t addr_len = sizeof(source_addr);

    while (m_rx_running) {
        int len = recvfrom(m_socket, buffer, sizeof(buffer), 0, (struct sockaddr*)&source_addr, &addr_len);
        if (len > 12) {
            auto snap = EmbeddedSysDb::getInstance().snapshot();
            if (!snap.audio.assistant_speaking && snap.pipeline.rtp_rx_en) {
                // Push payload (strip 12-byte RTP header) to speaker buffer
                BufferManager::getInstance().send(Buffers::SPK_RX_BUF, buffer + 12, len - 12);
            }
        }
    }
    m_rx_task_handle = nullptr;
    vTaskDelete(NULL);
}

void RtpTransceiver::closeSocket() {
    if (m_socket >= 0) {
        close(m_socket);
        m_socket = -1;
    }
}
