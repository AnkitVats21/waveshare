#pragma once

#include "IBluetoothSupervisor.h"
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <cstdio>
#include <array>
#include <memory>

namespace StarReplica {

/**
 * @brief Production Linux Bluetooth supervisor for Raspberry Pi Zero 2W.
 *
 * Monitors Bluetooth connection status of the Tribit A2DP sink via bluetoothctl / BlueZ,
 * and maintains automatic reconnection if the speaker is powered off or moves out of range.
 */
class BluezSupervisor : public IBluetoothSupervisor {
public:
    explicit BluezSupervisor(const std::string& target_mac = "")
        : m_target_mac(target_mac) {}

    ~BluezSupervisor() override {
        stop();
    }

    bool start(StateChangeCb cb) override {
        m_cb = std::move(cb);
        m_running = true;
        m_worker = std::thread(&BluezSupervisor::monitorLoop, this);
        std::cout << "[BluezSupervisor] Started monitoring Bluetooth speaker (target=" 
                  << (m_target_mac.empty() ? "any connected" : m_target_mac) << ")" << std::endl;
        return true;
    }

    void stop() override {
        m_running = false;
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    BluetoothDeviceState getCurrentState() const override {
        return m_state;
    }

private:
    std::string m_target_mac;
    StateChangeCb m_cb;
    BluetoothDeviceState m_state;
    std::atomic<bool> m_running{false};
    std::thread m_worker;

    static std::string execCmd(const char* cmd) {
        std::array<char, 256> buffer;
        std::string result;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
        if (!pipe) return "";
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            result += buffer.data();
        }
        return result;
    }

    void monitorLoop() {
        bool last_connected = false;

        while (m_running) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (!m_running) break;

            // Query connected bluetooth devices
            std::string out = execCmd("bluetoothctl devices Connected 2>/dev/null");
            bool is_connected = !out.empty();

            std::string name = "Tribit XSound Go";
            std::string mac = m_target_mac;

            if (is_connected) {
                // Parse "Device XX:XX:XX:XX:XX:XX Name"
                auto pos = out.find("Device ");
                if (pos != std::string::npos) {
                    size_t mac_start = pos + 7;
                    size_t space_pos = out.find(' ', mac_start);
                    if (space_pos != std::string::npos) {
                        mac = out.substr(mac_start, space_pos - mac_start);
                        size_t end_line = out.find('\n', space_pos);
                        if (end_line != std::string::npos) {
                            name = out.substr(space_pos + 1, end_line - space_pos - 1);
                        }
                    }
                }
            } else if (!m_target_mac.empty()) {
                // Try background reconnect if target MAC is configured
                std::string connect_cmd = "bluetoothctl connect " + m_target_mac + " >/dev/null 2>&1";
                execCmd(connect_cmd.c_str());
            }

            if (is_connected != last_connected) {
                last_connected = is_connected;
                m_state.connected = is_connected;
                m_state.device_name = is_connected ? name : "";
                m_state.mac_address = is_connected ? mac : "";
                m_state.rssi = is_connected ? -50 : 0;

                std::cout << "[BluezSupervisor] Connection status changed: " 
                          << (is_connected ? "CONNECTED (" + name + ")" : "DISCONNECTED") 
                          << std::endl;

                if (m_cb) {
                    m_cb(m_state);
                }
            }
        }
    }
};

} // namespace StarReplica
