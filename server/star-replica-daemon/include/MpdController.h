#pragma once

#include "IPlaybackController.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>

namespace StarReplica {

/**
 * @brief Production MPD controller communicating directly with local MPD daemon over TCP port 6600.
 *
 * Implements standard MPD protocol (zero external library dependencies).
 */
class MpdController : public IPlaybackController {
public:
    MpdController(const std::string& host = "127.0.0.1", uint16_t port = 6600)
        : m_host(host), m_port(port) {}

    ~MpdController() override {
        stop();
    }

    bool init(PositionUpdateCb pos_cb) override {
        m_pos_cb = std::move(pos_cb);
        m_running = true;
        m_poll_thread = std::thread(&MpdController::pollWorker, this);
        std::cout << "[MpdController] Initialized for MPD at " << m_host << ":" << m_port << std::endl;
        return true;
    }

    void handleCommand(PlaybackCmd cmd, uint32_t param, const std::string& data) override {
        switch (cmd) {
            case PlaybackCmd::PLAY: {
                std::ostringstream ss;
                ss << "clear\nadd " << data << "\nplay\n";
                if (param > 0) {
                    ss << "seekcur " << (param / 1000) << "\n";
                }
                sendCommand(ss.str());
                break;
            }
            case PlaybackCmd::PAUSE:
                sendCommand("pause 1\n");
                break;
            case PlaybackCmd::RESUME:
                sendCommand("pause 0\n");
                break;
            case PlaybackCmd::STOP:
                sendCommand("stop\n");
                break;
            case PlaybackCmd::NEXT:
                sendCommand("next\n");
                break;
            case PlaybackCmd::PREVIOUS:
                sendCommand("previous\n");
                break;
            case PlaybackCmd::SEEK: {
                std::ostringstream ss;
                ss << "seekcur " << (param / 1000) << "\n";
                sendCommand(ss.str());
                break;
            }
        }
    }

    void stop() override {
        m_running = false;
        if (m_poll_thread.joinable()) {
            m_poll_thread.join();
        }
        closeSocket();
    }

private:
    std::string m_host;
    uint16_t m_port;
    PositionUpdateCb m_pos_cb;
    std::atomic<bool> m_running{false};
    std::thread m_poll_thread;
    std::mutex m_sock_mutex;
    int m_sock_fd = -1;

    bool ensureConnected() {
        if (m_sock_fd >= 0) return true;

        m_sock_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (m_sock_fd < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(m_port);
        inet_pton(AF_INET, m_host.c_str(), &addr.sin_addr);

        if (connect(m_sock_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(m_sock_fd);
            m_sock_fd = -1;
            return false;
        }

        // Consume initial "OK MPD <version>\n"
        char buf[128];
        recv(m_sock_fd, buf, sizeof(buf) - 1, 0);
        return true;
    }

    void closeSocket() {
        std::lock_guard<std::mutex> lock(m_sock_mutex);
        if (m_sock_fd >= 0) {
            close(m_sock_fd);
            m_sock_fd = -1;
        }
    }

    bool sendCommand(const std::string& cmd) {
        std::lock_guard<std::mutex> lock(m_sock_mutex);
        if (!ensureConnected()) return false;

        if (send(m_sock_fd, cmd.c_str(), cmd.size(), 0) < 0) {
            close(m_sock_fd);
            m_sock_fd = -1;
            return false;
        }

        // Consume response
        char buf[256];
        recv(m_sock_fd, buf, sizeof(buf) - 1, 0);
        return true;
    }

    void pollWorker() {
        while (m_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!m_running) break;

            std::string resp;
            {
                std::lock_guard<std::mutex> lock(m_sock_mutex);
                if (!ensureConnected()) continue;

                const char* q = "status\n";
                if (send(m_sock_fd, q, strlen(q), 0) < 0) {
                    close(m_sock_fd);
                    m_sock_fd = -1;
                    continue;
                }

                char buf[1024];
                int n = recv(m_sock_fd, buf, sizeof(buf) - 1, 0);
                if (n > 0) {
                    buf[n] = '\0';
                    resp = buf;
                }
            }

            if (!resp.empty() && m_pos_cb) {
                // Parse elapsed: and duration:
                std::istringstream stream(resp);
                std::string line;
                float elapsed = 0.0f;
                float duration = 0.0f;

                while (std::getline(stream, line)) {
                    if (line.rfind("elapsed: ", 0) == 0) {
                        elapsed = std::stof(line.substr(9));
                    } else if (line.rfind("duration: ", 0) == 0) {
                        duration = std::stof(line.substr(10));
                    }
                }

                if (elapsed > 0.0f || duration > 0.0f) {
                    m_pos_cb(static_cast<uint32_t>(elapsed * 1000.0f),
                             static_cast<uint32_t>(duration * 1000.0f));
                }
            }
        }
    }
};

} // namespace StarReplica
