#pragma once

#include "common/TaskBase.h"
#include "common/app_types.h"

// Sends the session's mic audio (MIC_TX_BUF) to the voice backend
// (VoiceAgent::active()). Core 0: the send is network work.
class VoiceUplinkPump : public TaskBase {
public:
    static VoiceUplinkPump& getInstance();
    bool start();

protected:
    void run() override;

private:
    VoiceUplinkPump(const Config& cfg);
    ~VoiceUplinkPump() override = default;

    bool processUplink();

    WsState      m_cached_ws_state      = WsState::DISCONNECTED;
    PipelineMode m_cached_pipeline_mode = PipelineMode::WAKE_IDLE;
    static constexpr const char* TAG = "VoiceUplinkPump";
};
