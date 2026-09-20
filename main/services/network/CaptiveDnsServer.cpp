#include "services/network/CaptiveDnsServer.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <cstring>

static const char* TAG = "CaptiveDns";

CaptiveDnsServer& CaptiveDnsServer::getInstance() {
    static CaptiveDnsServer instance;
    return instance;
}

bool CaptiveDnsServer::start(uint32_t redirect_ip_be) {
    if (m_running.load()) {
        return true;
    }

    m_redirect_ip = redirect_ip_be;
    m_running.store(true);

    BaseType_t res = xTaskCreatePinnedToCore(
        &CaptiveDnsServer::taskFunc,
        "captive_dns",
        3072,
        this,
        3,
        &m_task_handle,
        0
    );

    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create captive DNS task");
        m_running.store(false);
        return false;
    }

    ESP_LOGI(TAG, "Captive DNS server started on UDP port 53");
    return true;
}

void CaptiveDnsServer::stop() {
    if (!m_running.load()) {
        return;
    }

    m_running.store(false);

    if (m_sock >= 0) {
        // Shutdown and close socket to wake up blocking recvfrom
        shutdown(m_sock, SHUT_RDWR);
        close(m_sock);
        m_sock = -1;
    }

    if (m_task_handle != nullptr) {
        // Allow task up to 200ms to exit cleanly
        for (int i = 0; i < 20 && eTaskGetState(m_task_handle) != eDeleted; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        m_task_handle = nullptr;
    }

    ESP_LOGI(TAG, "Captive DNS server stopped");
}

void CaptiveDnsServer::taskFunc(void* arg) {
    static_cast<CaptiveDnsServer*>(arg)->run();
    vTaskDelete(nullptr);
}

void CaptiveDnsServer::run() {
    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (m_sock < 0) {
        ESP_LOGE(TAG, "Unable to create UDP socket: errno %d", errno);
        m_running.store(false);
        return;
    }

    // Set receive timeout so the task can gracefully notice termination
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in saddr = {};
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    saddr.sin_port = htons(53);

    if (bind(m_sock, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
        ESP_LOGE(TAG, "Socket unable to bind to port 53: errno %d", errno);
        close(m_sock);
        m_sock = -1;
        m_running.store(false);
        return;
    }

    uint8_t rx_buffer[512];
    uint8_t tx_buffer[512];

    while (m_running.load()) {
        struct sockaddr_in client_addr = {};
        socklen_t addr_len = sizeof(client_addr);

        ssize_t len = recvfrom(m_sock, rx_buffer, sizeof(rx_buffer), 0,
                               (struct sockaddr*)&client_addr, &addr_len);

        if (len < 12) {
            // Timeout or runt packet
            continue;
        }

        // Parse DNS query header
        // Header is 12 bytes:
        // ID(2), Flags(2), QDCOUNT(2), ANCOUNT(2), NSCOUNT(2), ARCOUNT(2)
        std::memcpy(tx_buffer, rx_buffer, len);

        // Set Flags: Standard query response, No error (0x8180)
        tx_buffer[2] = 0x81;
        tx_buffer[3] = 0x80;

        // Set Answer Count: 1
        tx_buffer[6] = 0x00;
        tx_buffer[7] = 0x01;

        // Set Authority and Additional records: 0
        tx_buffer[8] = 0x00;
        tx_buffer[9] = 0x00;
        tx_buffer[10] = 0x00;
        tx_buffer[11] = 0x00;

        // Append 16-byte Answer record pointing to the question name
        size_t resp_len = len;
        if (resp_len + 16 <= sizeof(tx_buffer)) {
            // Name: Compressed pointer to question name at offset 12 (0xC00C)
            tx_buffer[resp_len++] = 0xC0;
            tx_buffer[resp_len++] = 0x0C;

            // Type: A (Host address = 1)
            tx_buffer[resp_len++] = 0x00;
            tx_buffer[resp_len++] = 0x01;

            // Class: IN (Internet = 1)
            tx_buffer[resp_len++] = 0x00;
            tx_buffer[resp_len++] = 0x01;

            // TTL: 60 seconds (0x0000003C)
            tx_buffer[resp_len++] = 0x00;
            tx_buffer[resp_len++] = 0x00;
            tx_buffer[resp_len++] = 0x00;
            tx_buffer[resp_len++] = 0x3C;

            // Data length: 4 bytes (IPv4)
            tx_buffer[resp_len++] = 0x00;
            tx_buffer[resp_len++] = 0x04;

            // IP Address bytes (e.g. 192.168.4.1)
            std::memcpy(&tx_buffer[resp_len], &m_redirect_ip, 4);
            resp_len += 4;

            sendto(m_sock, tx_buffer, resp_len, 0,
                   (struct sockaddr*)&client_addr, addr_len);
        }
    }

    if (m_sock >= 0) {
        close(m_sock);
        m_sock = -1;
    }
}
