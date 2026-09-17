#pragma once

#include "InvidiousClient.h"
#include "app/media_player/NexusPlayer.h"
#include "app/media_player/IPlaybackObserver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string>
#include <deque>
#include <vector>
#include <mutex>

enum class RepeatMode {
    Off,
    One,
    All
};

enum class MediaCmdType : uint8_t {
    PLAY,
    PLAY_NEXT,
    QUEUE,
    NEXT,
    PREVIOUS,
    PAUSE,
    RESUME,
    STOP,
    TOGGLE_PLAY_PAUSE,
    REPLAY
};

struct MediaCommand {
    MediaCmdType type;
    uint32_t generation;
    char query[256];
};

enum class MediaAuxCmdType : uint8_t {
    PREFETCH,
    REPLENISH
};

struct MediaAuxCommand {
    MediaAuxCmdType type;
    uint32_t generation;
    char targetId[64];
    char baseAuthor[128];
    char baseTitle[128];
};

class MusicPlaybackService : public IPlaybackObserver {
public:
    static MusicPlaybackService& getInstance();

    bool begin();
    
    // Asynchronous dispatch via persistent command queue
    bool postCommand(MediaCmdType type, const char* query = nullptr);

    // Convenience API wrappers for backward compatibility and simplicity
    bool play(const char* query);
    bool playDirect(const InvidiousTrack& track, const char* streamUrl);
    bool playLocal(const char* songIdOrPath);
    bool playNext(const char* query);
    bool queue(const char* query);
    bool next();
    bool previous();
    void pause();
    void resume();
    void stop();

    // Playlist / Queue Management
    void clearQueue();
    void shuffleQueue();
    void setRepeatMode(RepeatMode mode);
    RepeatMode getRepeatMode() const { return _repeatMode; }
    void setAutoplay(bool enabled);
    bool isAutoplayEnabled() const;
    void setCaching(bool enabled);
    bool isCachingEnabled() const;

    InvidiousTrack getCurrentTrack() const {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        return _currentTrack;
    }
    std::deque<InvidiousTrack> getQueue() const {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        return _queue;
    }
    std::vector<InvidiousTrack> getHistory() const {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        return _history;
    }

    InvidiousClient& getInvidiousClient() { return _invidious; }
    void populateRecommendations(const std::vector<InvidiousTrack>& recs, const std::string& title);

    // IPlaybackObserver implementation
    void onTrackStarted(const char* songId) override;
    void onTrackFinished(const char* songId) override;
    void onPlaybackError(const char* songId, int errorCode) override;

    static constexpr size_t QUEUE_LOW_WATERMARK = 2;
    // Upper bound on consecutive unavailable tracks to skip in a single advance
    // before giving up, so a run of dead video IDs can't spin forever.
    static constexpr int MAX_SKIP_ON_ADVANCE = 6;

private:
    MusicPlaybackService();
    ~MusicPlaybackService() override = default;

    InvidiousClient _invidious;
    bool _initialized = false;
    bool _autoplayEnabled = true;
    RepeatMode _repeatMode = RepeatMode::Off;

    mutable std::recursive_mutex _serviceMutex;
    InvidiousTrack _currentTrack;
    std::deque<InvidiousTrack> _queue;
    std::vector<InvidiousTrack> _history;

    volatile bool _prefetchInProgress = false;
    volatile bool _replenishInProgress = false;
    uint32_t _queueGeneration = 0;
    std::string _prefetchedVideoId;
    std::string _prefetchedUrl;

    // Persistent concurrency queues and worker tasks
    QueueHandle_t m_cmd_queue = nullptr;
    TaskHandle_t m_worker_task = nullptr;
    QueueHandle_t m_aux_queue = nullptr;
    TaskHandle_t m_aux_task = nullptr;

    static void workerTask(void* arg);
    static void auxWorkerTask(void* arg);
    void workerLoop();
    void auxWorkerLoop();

    bool playTrack(const InvidiousTrack& track);
    // Resolve + start a track. Returns:
    //   ESP_OK            - playback started
    //   ESP_ERR_NOT_FOUND - video is gone/private/region-blocked; caller should skip it
    //   ESP_FAIL          - transient network/instance error; caller should stop and let retry/backoff handle it
    esp_err_t playTrackInternal(const InvidiousTrack& track);
    // Advance to the next playable track, skipping up to MAX_SKIP_ON_ADVANCE
    // consecutive unavailable entries. Handles autoplay/recommendation refill.
    bool advanceToNextPlayable();
    // Bump the queue generation so in-flight background prefetch/replenish tasks
    // discard their results instead of racing a user-initiated track change.
    void invalidateBackgroundWork();
    bool playTrackFallback(const InvidiousTrack& track);
    bool resolveAndPlayImmediate(const char* query);
    bool playNextInternal(const char* query);
    bool queueInternal(const char* query);
    bool previousInternal();
    bool nextInternal();

    void prefetchNextTrack();
    void checkAndReplenishQueue();
    void handlePrefetch(const char* targetId, uint32_t generation);
    void handleReplenish(const char* baseId, const char* baseAuthor, const char* baseTitle, uint32_t generation);
    bool isTrackInQueueOrHistory(const std::string& videoId) const;
};
