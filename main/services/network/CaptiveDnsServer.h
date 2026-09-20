#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>
#include <atomic>

/**
 * @brief Lightweight DNS responder that redirects all domain queries to the SoftAP IP (192.168.4.1).
 *
 * This enables automatic captive portal detection on iOS, Android, macOS, and Windows.
 */
class CaptiveDnsServer {
public:
    static CaptiveDnsServer& getInstance();

    bool start(uint32_t redirect_ip_be = 0x0104A8C0); // 192.168.4.1 in network byte order
    void stop();
    bool isRunning() const { return m_running.load(); }

private:
    CaptiveDnsServer() = default;
    ~CaptiveDnsServer() { stop(); }

    CaptiveDnsServer(const CaptiveDnsServer&) = delete;
    CaptiveDnsServer& operator=(const CaptiveDnsServer&) = delete;

    static void taskFunc(void* arg);
    void run();

    std::atomic<bool> m_running{false};
    int               m_sock = -1;
    TaskHandle_t      m_task_handle = nullptr;
    uint32_t          m_redirect_ip = 0;
};
