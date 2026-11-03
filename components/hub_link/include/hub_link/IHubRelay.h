#pragma once

#include <cstddef>
#include <string>

namespace Services {

// The dashboard's channel through the hub (main: ControlChannel). HubLink
// hands it a way to send, says when the link is up, and passes on the
// {"cmd"} frames the hub relays.
class IHubRelay {
public:
    virtual ~IHubRelay() = default;
    typedef bool (*SendFn)(const std::string& text, void* ctx);
    virtual void setRelay(SendFn send, void* ctx) = 0;
    virtual void setRelayUp(bool up) = 0;
    virtual void handleRelayCommand(const char* json, size_t len) = 0;
};

// The settings the hub link reads (main: system.ndb), at each connect and
// each session start.
struct HubSettings {
    std::string url;                   // empty: find the hub by mDNS
    uint8_t uplink_opus_kbps = 24;
    uint8_t uplink_opus_complexity = 3;
};
typedef HubSettings (*HubSettingsFn)();

} // namespace Services
