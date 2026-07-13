#pragma once

#include "common/TaskBase.h"
#include "common/thread_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "cJSON.h"
#include "esp_http_client.h"

#define STREAM_START_BIT            (1 << 0)
#define PLAYER_STOP_BIT             (1 << 1)
#define PLAYER_PAUSE_BIT            (1 << 2)
#define PLAYER_STREAM_PENDING_BIT   (1 << 3)
#define PLAYER_STOPPED_ACK_BIT      (1 << 4)
#define PLAYER_RESUME_BIT           (1 << 5)

enum PlayerState {
    PLAYER_IDLE,
    PLAYER_FETCHING,
    PLAYER_PAUSED,
    PLAYER_ASSISTANT_ACTIVE,
};

struct chunk_manifest_t {
    char token[65];
    char manifest_url[256];
    char video_id[32];
    char title[128];
    uint32_t from_chunk;
    int total_chunks;
    bool valid;
};

struct pending_stream_t {
    char manifest_url[256];
    char video_id[32];
    char title[128];
    bool valid;
};

class HttpStreamService : public TaskBase {
public:
    static HttpStreamService& getInstance();

    HttpStreamService();
    ~HttpStreamService() override;

    bool begin();

    void startStream(const cJSON* payload);
    void pauseStream();
    void resumeStream();
    void stopStream();
    void handleServerOnline();
    
    void suspendTask();
    void resumeTask();

    PlayerState getState() const { return m_state; }

protected:
    void run() override;

private:
    struct http_chunk_result_t {
        int status;
        size_t bytes_read;
        bool is_last_chunk;
    };

    bool httpGetManifest(const char* url, chunk_manifest_t* out_manifest);
    esp_http_client_handle_t httpClientInit(const char* token);
    http_chunk_result_t httpGetChunk(esp_http_client_handle_t client, const char* token, uint32_t index, uint8_t* buf, size_t buf_size);
    
    void applyPendingStream();
    void waitForBufferDrain(uint32_t timeout_ms);
    void publishPauseCommand(uint32_t last_chunk);
    void publishReplayCommand(const char* video_id);

    EventGroupHandle_t m_event_group = nullptr;
    volatile PlayerState m_state = PLAYER_IDLE;
    
    chunk_manifest_t m_manifest = {};
    pending_stream_t m_pending = {};
    uint32_t m_next_chunk = 0;

    static constexpr const char* TAG = "HttpStreamSvc";
};
