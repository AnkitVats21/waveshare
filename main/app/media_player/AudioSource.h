#pragma once

#include "common/TaskBase.h"
#include <string>
#include <atomic>
#include <cstdint>
#include <cstddef>

enum class ChunkType : uint8_t {
    DATA,
    EOF_STREAM,
    ERROR
};

struct AudioChunkHeader {
    ChunkType type;
    uint32_t size;
};

static constexpr size_t AUDIO_CHUNK_SIZE = 32 * 1024;

class AudioSource : public TaskBase {
public:
    AudioSource(const char* name, uint32_t stack_size, UBaseType_t priority, BaseType_t core_id);
    ~AudioSource() override = default;

    virtual void stopSource();
    virtual void resetSource();

    bool isStopped() const { return m_stop.load(); }

protected:
    std::atomic<bool> m_stop{false};
};

class SdCardAudioSource : public AudioSource {
public:
    SdCardAudioSource();
    ~SdCardAudioSource() override;

    bool setFilePath(const char* path);
    void resetSource() override;

protected:
    void run() override;

private:
    std::string m_path;
    uint8_t* m_chunk_buf = nullptr;
    static constexpr const char* TAG = "SdCardAudioSource";
};

class HttpAudioSource : public AudioSource {
public:
    HttpAudioSource();
    ~HttpAudioSource() override;

    bool setUrl(const char* url);
    void resetSource() override;

protected:
    void run() override;

private:
    std::string m_url;
    uint8_t* m_chunk_buf = nullptr;
    static constexpr const char* TAG = "HttpAudioSource";
};
