#pragma once

#include "common/TaskBase.h"
#include "common/app_types.h"

class GeminiAudioPump : public TaskBase {
public:
    static GeminiAudioPump& getInstance();

protected:
    void run() override;

private:
    GeminiAudioPump(const Config& cfg);
    ~GeminiAudioPump() override = default;

    bool processUplink();

    char* m_static_b64_arena = nullptr;
    WsState      m_cached_ws_state      = WsState::DISCONNECTED;
    PipelineMode m_cached_pipeline_mode = PipelineMode::WAKE_IDLE;
    static constexpr const char* TAG = "GeminiAudioPump";
};



