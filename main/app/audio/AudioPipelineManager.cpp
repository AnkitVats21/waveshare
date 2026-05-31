#include "AudioPipelineManager.h"
#include "MicCapture.h"
#include "RtpReceiver.h"
#include "RtpStreamer.h"
#include "SpeakerPlayback.h"
#include "common/AppLogger.h"
#include "services/BufferManager.h"
#include "lwip/sockets.h"
#include <cstring>

MicCaptureTask*      AudioPipelineManager::m_mic_task     = nullptr;
SpeakerPlaybackTask* AudioPipelineManager::m_speaker_task = nullptr;
RtpStreamer*         AudioPipelineManager::m_rtp_tx       = nullptr;
RtpReceiver*         AudioPipelineManager::m_rtp_rx       = nullptr;
int                  AudioPipelineManager::m_shared_socket = -1;

bool AudioPipelineManager::initialize(const GlobalSystemSettings &settings,
                                      const HardwareAudioHandles &hw_handles) {

    LOGI_AUDIO("Initializing Audio Pipeline (Sample Rate: %lu Hz)...", settings.sample_rate);

    // Verify ring buffers are allocated (done in SystemContext::init via BufferManager::initAll)
    if (!BufferManager::getInstance().handle(Buffers::MIC_TX_BUF) ||
        !BufferManager::getInstance().handle(Buffers::SPK_RX_BUF)) {
        LOGE_AUDIO("Ring buffers not allocated — was BufferManager::initAll() called?");
        return false;
    }

    // 1. Start Hardware Tasks
    m_mic_task = new MicCaptureTask();
    m_mic_task->start(settings, hw_handles.mic_rx_handle);
#ifdef CONFIG_WAVESHARE_WAKEWORD_ENABLE
    // If WakeWordDetector is active, its feedTask takes exclusive ownership of the mic.
    // We soft-disable MicCaptureTask to prevent concurrent I2S reads from corrupting the driver state.
    m_mic_task->setEnabled(false);
#endif

    m_speaker_task = new SpeakerPlaybackTask();
    m_speaker_task->start(settings, hw_handles.play_dev);

    // 2. Create Shared Bidirectional UDP Socket
    m_shared_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (m_shared_socket < 0) {
        LOGE_AUDIO("Failed to create shared RTP UDP socket!");
        return false;
    }

    struct sockaddr_in bind_addr;
    std::memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(settings.rx_rtp_port);

    if (bind(m_shared_socket, reinterpret_cast<struct sockaddr *>(&bind_addr),
             sizeof(bind_addr)) < 0) {
        LOGE_AUDIO("Failed to bind shared UDP socket on port %d!", settings.rx_rtp_port);
        close(m_shared_socket);
        m_shared_socket = -1;
        return false;
    }

    struct timeval timeout;
    timeout.tv_sec  = 0;
    timeout.tv_usec = 200000;
    if (setsockopt(m_shared_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) < 0) {
        LOGE_AUDIO("Failed to set receive timeout on shared UDP socket!");
    }

    // 3. Start RTP Transmitter
    RtpStreamer::TxConfig tx_cfg;
    tx_cfg.target_ip = settings.server_ip;
    tx_cfg.port      = settings.tx_rtp_port;
    tx_cfg.format    = settings.stream_format;
    tx_cfg.priority  = settings.tx_priority;
    tx_cfg.stack_size = 4096;
    tx_cfg.core_id   = settings.network_core_id;

    m_rtp_tx = new RtpStreamer(tx_cfg, Buffers::MIC_TX_BUF, m_shared_socket);
    if (!m_rtp_tx->begin()) {
        LOGE_AUDIO("Failed to start RTP Streamer");
    }

    // 4. Start RTP Receiver
    RtpTaskBase::CommonConfig rx_cfg;
    rx_cfg.port       = settings.rx_rtp_port;
    rx_cfg.priority   = settings.rx_priority;
    rx_cfg.stack_size = 4096;
    rx_cfg.core_id    = settings.network_core_id;

    m_rtp_rx = new RtpReceiver(rx_cfg, Buffers::SPK_RX_BUF, m_shared_socket, "rtp_receiver");
    if (!m_rtp_rx->begin()) {
        LOGE_AUDIO("Failed to start RTP Receiver");
    }

    return true;
}

void AudioPipelineManager::teardown() {
    LOGI_AUDIO("Tearing down Audio Pipeline...");

    // 1. Stop network tasks first
    if (m_rtp_tx) { m_rtp_tx->stop(); delete m_rtp_tx; m_rtp_tx = nullptr; }
    if (m_rtp_rx) { m_rtp_rx->stop(); delete m_rtp_rx; m_rtp_rx = nullptr; }

    // 2. Close shared socket
    if (m_shared_socket >= 0) {
        close(m_shared_socket);
        m_shared_socket = -1;
        LOGI_AUDIO("Shared UDP socket closed.");
    }

    // 3. Stop hardware tasks
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

    // 4. Flush ring buffers (BufferManager owns them; do NOT delete)
    BufferManager::getInstance().flush(Buffers::MIC_TX_BUF);
    BufferManager::getInstance().flush(Buffers::SPK_RX_BUF);

    LOGI_AUDIO("Audio Pipeline teardown complete.");
}

void AudioPipelineManager::setMicEnabled(bool enabled) {
    if (m_mic_task) m_mic_task->setEnabled(enabled);
    if (m_rtp_tx)   m_rtp_tx->setEnabled(enabled);
    LOGI_AUDIO("Mic pipeline %s (Task + RTP)", enabled ? "ENABLED" : "SOFT-DISABLED");
}

void AudioPipelineManager::setRtpEnabled(bool enabled) {
    if (m_rtp_tx) m_rtp_tx->setEnabled(enabled);
    LOGI_AUDIO("RTP streamer %s", enabled ? "ENABLED" : "DISABLED");
}

void AudioPipelineManager::setRtpRxInterrupted(bool interrupted) {
    if (m_rtp_rx) m_rtp_rx->setInterrupted(interrupted);
}
