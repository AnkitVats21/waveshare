#pragma once

#include "common/ReactorTask.h"
#include "common/thread_config.h"
#include "services/BufferManager.h"
#include "lwip/sockets.h"
#include <array>
#include <cstdint>

/**
 * @brief Decoupled RTP Player Service.
 *
 * Fetches RTP audio packets (expected to be 16kHz mono PCM) from a UDP socket
 * and feeds them to the speaker playback buffer. Runs only when WiFi is connected
 * and the voice assistant is idle.
 *
 * Watches:
 *   COMP::SYSTEM    — wifi_connected
 *   COMP::ASSISTANT — session_state
 */
class RtpPlayer : public ReactorTask {
public:
    RtpPlayer();
    virtual ~RtpPlayer();

    bool begin();

    // ReactorTask interface
    void onStateChanged(ComponentMask changed, const SystemState& snap) override;

protected:
    void run() override;

private:
    int m_socket = -1;
    TaskHandle_t m_rx_task_handle = nullptr;
    volatile bool m_rx_running = false;

    // Local cached settings
    uint16_t m_rx_port = 0;
    bool     m_seq_initialized = false;
    uint16_t m_last_seq = 0;
    size_t   m_last_payload_bytes = 0;
    uint32_t m_packets_received = 0;
    uint32_t m_packets_concealed = 0;
    uint32_t m_packets_dropped = 0;
    uint32_t m_packets_reordered = 0;
    std::array<uint8_t, 1500> m_rx_buffer = {};
    std::array<uint8_t, 1500> m_conceal_buffer = {};

    void startPlayer(const SystemState& snap);
    void stopPlayer();

    // Receiver worker
    static void rxWorkerBridge(void* arg);
    void rxLoop();

    void closeSocket();

    static constexpr const char* TAG = "RtpPlayer";
};
