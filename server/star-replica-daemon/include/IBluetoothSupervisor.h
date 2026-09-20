#pragma once

#include <string>
#include <functional>
#include <cstdint>

namespace StarReplica {

struct BluetoothDeviceState {
    bool connected = false;
    std::string device_name;
    std::string mac_address;
    int8_t rssi = 0;
};

class IBluetoothSupervisor {
public:
    virtual ~IBluetoothSupervisor() = default;

    using StateChangeCb = std::function<void(const BluetoothDeviceState& state)>;

    virtual bool start(StateChangeCb cb) = 0;
    virtual void stop() = 0;
    virtual BluetoothDeviceState getCurrentState() const = 0;
};

} // namespace StarReplica
