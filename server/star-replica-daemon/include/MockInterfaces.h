#pragma once

#include "IBluetoothSupervisor.h"
#include "IPlaybackController.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>

namespace StarReplica {

class MockBluetoothSupervisor : public IBluetoothSupervisor {
public:
    bool start(StateChangeCb cb) override {
        m_cb = std::move(cb);
        std::cout << "[MockBT] Started (Simulated Bluetooth supervisor ready)" << std::endl;
        return true;
    }

    void stop() override {
        std::cout << "[MockBT] Stopped" << std::endl;
    }

    BluetoothDeviceState getCurrentState() const override {
        return m_state;
    }

    void simulateConnection(bool connected, const std::string& name = "Tribit XSound Go", const std::string& mac = "AA:BB:CC:DD:EE:FF") {
        m_state.connected = connected;
        m_state.device_name = connected ? name : "";
        m_state.mac_address = connected ? mac : "";
        m_state.rssi = connected ? -55 : 0;
        std::cout << "[MockBT] Simulated device state changed: connected=" << connected << " (" << name << ")" << std::endl;
        if (m_cb) {
            m_cb(m_state);
        }
    }

private:
    StateChangeCb m_cb;
    BluetoothDeviceState m_state;
};

class MockPlaybackController : public IPlaybackController {
public:
    bool init(PositionUpdateCb pos_cb) override {
        m_pos_cb = std::move(pos_cb);
        std::cout << "[MockMPD] Initialized (Simulated MPD player ready)" << std::endl;
        return true;
    }

    void handleCommand(PlaybackCmd cmd, uint32_t param, const std::string& data) override {
        switch (cmd) {
            case PlaybackCmd::PLAY:
                std::cout << "[MockMPD] PLAY url=" << data << " offset=" << param << "ms" << std::endl;
                m_playing = true;
                m_position_ms = param;
                break;
            case PlaybackCmd::PAUSE:
                std::cout << "[MockMPD] PAUSE" << std::endl;
                m_playing = false;
                break;
            case PlaybackCmd::RESUME:
                std::cout << "[MockMPD] RESUME" << std::endl;
                m_playing = true;
                break;
            case PlaybackCmd::STOP:
                std::cout << "[MockMPD] STOP" << std::endl;
                m_playing = false;
                m_position_ms = 0;
                break;
            case PlaybackCmd::SEEK:
                std::cout << "[MockMPD] SEEK to " << param << "ms" << std::endl;
                m_position_ms = param;
                break;
            default:
                break;
        }
    }

    void stop() override {
        m_playing = false;
        std::cout << "[MockMPD] Stopped" << std::endl;
    }

private:
    PositionUpdateCb m_pos_cb;
    bool m_playing = false;
    uint32_t m_position_ms = 0;
};

} // namespace StarReplica
