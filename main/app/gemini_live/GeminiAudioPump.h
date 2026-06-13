#pragma once

#include "common/TaskBase.h"

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
    static constexpr const char* TAG = "GeminiAudioPump";
};


