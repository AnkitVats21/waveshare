#pragma once

#include "common/TaskBase.h"

/**
 * @brief Pinned to Core 1, manages the uncompressed 16kHz audio uplink 
 * and mbedtls Base64 transcoding for ultra-low latency streams.
 */
class GeminiAudioPumpTask : public TaskBase {
public:
    static GeminiAudioPumpTask& getInstance();

protected:
    void run() override;

private:
    GeminiAudioPumpTask(const Config& cfg);
    ~GeminiAudioPumpTask() = default;

    bool processUplink();

    char* m_static_b64_arena = nullptr;
};
