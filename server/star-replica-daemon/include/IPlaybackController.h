#pragma once

#include <string>
#include <cstdint>
#include <functional>

namespace StarReplica {

enum class PlaybackCmd {
    PLAY,
    PAUSE,
    RESUME,
    STOP,
    NEXT,
    PREVIOUS,
    SEEK
};

class IPlaybackController {
public:
    virtual ~IPlaybackController() = default;

    using PositionUpdateCb = std::function<void(uint32_t position_ms, uint32_t duration_ms)>;

    virtual bool init(PositionUpdateCb pos_cb) = 0;
    virtual void handleCommand(PlaybackCmd cmd, uint32_t param, const std::string& data) = 0;
    virtual void stop() = 0;
};

} // namespace StarReplica
