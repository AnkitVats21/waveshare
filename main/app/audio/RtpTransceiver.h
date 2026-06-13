#pragma once

#include "common/ReactorTask.h"
#include "common/thread_config.h"
#include "services/BufferManager.h"
#include "lwip/sockets.h"

/**
 * @brief Unified RTP Transceiver Service.
 *
 * Replaces RtpStreamer and RtpReceiver with a single, reactive service.
 * Manages a shared UDP socket for bidirectional RTP traffic.
 *
 * Watches:
 *   COMP::SYSTEM   — wifi_connected
 *   COMP::PIPELINE — mode (RTP_REMOTE, RTP_WAKEWORD) and rtp_enabled gate
 */
class RtpTransceiver : public ReactorTask {
public:
    RtpTransceiver();
    virtual ~RtpTransceiver();

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
    std::string m_target_ip;
    uint16_t m_tx_port = 0;
    uint16_t m_rx_port = 0;

    void startTransceiver(const SystemState& snap);
    void stopTransceiver();

    // Receiver worker
    static void rxWorkerBridge(void* arg);
    void rxLoop();

    void closeSocket();

    static constexpr const char* TAG = "RtpTrans";
};
