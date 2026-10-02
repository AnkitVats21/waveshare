#pragma once

#include "TrackSource.h"
#include "sdkconfig.h"
#if CONFIG_WAVESHARE_INVIDIOUS_FALLBACK
#include "InvidiousClient.h"
#endif
#include "RemoteOutput.h"
#include "MusicHub.h"
#include "app/media_player/NexusPlayer.h"
#include "app/media_player/IPlaybackObserver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <functional>
#include <string>
#include <deque>
#include <vector>
#include <mutex>
#include <atomic>

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
    REPLAY,
    SEEK,
    SWITCH_OUTPUT, // query "1": to the satellite, "0": to the board
    HUB_PLAY       // play the song the hub sent (hubPlay)
};

struct MediaCommand {
    MediaCmdType type;
    uint32_t generation;
    char query[256];
};

enum class MediaAuxCmdType : uint8_t {
    PREFETCH,
    REPLENISH,
    FETCH_THUMBNAIL,
    RENEW_URL      // generation carries the renewal's request number instead
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

    // A command from the person (keys, dashboard, voice, API). In hub mode it
    // goes to the hub as an action, else to the media worker (dispatch).
    bool postCommand(MediaCmdType type, const char* query = nullptr);

    // Convenience API wrappers for backward compatibility and simplicity
    bool play(const char* query);
    bool playDirect(const InvidiousTrack& track, const char* streamUrl);
    bool playLocal(const char* songIdOrPath);
    // Plays a file outside the library (a recording) by its full path. The
    // track's id must start with FILE_TRACK_PREFIX. Kept out of the library,
    // history and autoplay; playback stops when it ends.
    static constexpr const char* FILE_TRACK_PREFIX = "file:";
    bool playFile(const InvidiousTrack& track, const char* path);
    // Stops playback if the file track `id` is playing (before deleting it).
    void stopFileTrack(const std::string& id);
    // Deletes a library song's saved file and thumbnail; its entry stays, as
    // not saved. Stops playback first if it is the current track.
    bool deleteSaved(const std::string& id);
    bool playNext(const char* query);
    bool queue(const char* query);
    bool next();
    bool previous();
    void pause();
    void resume();
    void stop();
    bool seekTo(uint32_t positionMs);
    uint32_t getPositionMs() const;

    // Satellite playback (nexus-orbit). main sets the output once at startup.
    // While media.output_target is SATELLITE, play/pause/seek/stop go to the
    // active satellite instead of NexusPlayer; the queue stays here.
    void setRemoteOutput(RemoteOutput* remote) { _remote = remote; }
    RemoteOutput* remoteOutput() const { return _remote; }
    // Moves the music to the active satellite (true) or the board (false),
    // carrying the current song and position. Queued to the media worker.
    bool switchOutput(bool satellite);
    // The assistant session started or ended (from NexusPlayer). Music on a
    // satellite pauses for the session, as on the board.
    void onAssistantSession(bool active);

    // Hub mode (MusicHub.h). main sets the hub at startup; while it is
    // active, the person's actions go to it and the board plays what it
    // sends. The hub* calls below come from the link (must not block).
    void setMusicHub(MusicHub* hub) { _hub = hub; }
    bool hubActive() const { return _hub && _hub->active(); }
    // Asks the hub to move the music to another speaker (music_output).
    bool hubOutput(const char* target);
    void hubPlay(const InvidiousTrack& track, const std::string& url, uint32_t positionMs, bool paused, uint32_t seq);
    void hubPause();
    void hubResume();
    void hubSeek(uint32_t positionMs);
    void hubStop();
    // The hub's queue (what plays next), repeat and autoplay.
    void hubQueue(std::deque<InvidiousTrack> tracks, RepeatMode repeat, bool autoplay);
    // The music_play the board is on; 0 when it chose the song itself.
    uint32_t hubSeq() const { return _hubSeq.load(); }
    // The board told the hub what it plays (music_sync): its song is now its
    // own (seq 0) for the hub, whatever the hub sent before.
    void hubSynced() { _hubSeq = 0; }

    // Playlist / Queue Management
    // Adds an already-identified track (no search). front = play next.
    void enqueueTrack(const InvidiousTrack& track, bool front);
    // Removes the queue entry at index; false if out of range.
    bool removeFromQueue(size_t index);
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

    // Where songs are looked up. main sets the MCP source at startup;
    // until then (or without it) the Invidious fallback, if built.
    void setTrackSource(TrackSource* source) { _source = source; }
    TrackSource& trackSource() { return *_source; }
    // The board's own Invidious client, or nullptr when not built.
    TrackSource* fallbackTrackSource();
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
    // The media worker's command queue (postCommand without the hub).
    bool dispatch(MediaCmdType type, const char* query = nullptr);
    bool forwardToHub(MediaCmdType type, const char* query);
    void clearQueueLocal();
    void playFromHub();
    MusicHub* _hub = nullptr;
    std::atomic<uint32_t> _hubSeq{0};
    std::mutex _hubMutex;               // the pending hub play
    InvidiousTrack _hubTrack;
    std::string _hubUrl;
    uint32_t _hubPos = 0;
    bool _hubPaused = false;
    uint32_t _hubPendingSeq = 0;

    void endFileTrack();
    bool isRemote(const SystemState& snap) const;
    void switchOutputInternal(bool satellite);
    void handoffToLocal(const std::string& songId, uint32_t posMs, bool paused);
    MusicPlaybackService();
    ~MusicPlaybackService() override = default;

#if CONFIG_WAVESHARE_INVIDIOUS_FALLBACK
    InvidiousClient _invidious;
#endif
    TrackSource* _source;
    RemoteOutput* _remote = nullptr;
    // Guarded by _serviceMutex: the assistant session is on, and music on the
    // satellite was paused for it (or asked to start during it), so it
    // resumes when the session ends.
    bool _sessionActive = false;
    bool _remoteHeldForSession = false;
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

    // Stream URL renewal (an expired URL after a long pause): the network
    // task asks, media_aux resolves (it has the stack for HTTPS + JSON), and
    // the network task waits for the answer to its request number.
    std::mutex _renewMutex;
    uint32_t _renewSeq = 0;
    bool _renewDone = false;
    bool _renewOk = false;
    std::string _renewUrl;
    bool renewStreamUrl(const std::string& videoId, std::string& outUrl, const std::function<bool()>& cancelled);
    void handleRenewUrl(const char* videoId, uint32_t seq);

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

    bool postAuxCommand(MediaAuxCmdType type, const char* targetId, const char* author = nullptr, const char* title = nullptr);
    void prefetchNextTrack();
    void checkAndReplenishQueue();
    void handlePrefetch(const char* targetId, uint32_t generation);
    void handleReplenish(const char* baseId, const char* baseAuthor, const char* baseTitle, uint32_t generation);
    bool isTrackInQueueOrHistory(const std::string& videoId) const;
};
