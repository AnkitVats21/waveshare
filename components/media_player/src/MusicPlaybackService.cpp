#include "MusicPlaybackService.h"
#include "media_player/CatalogDB.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include <algorithm>
#include <random>
#include <cstring>
#include <sys/stat.h>

static const char* TAG = "MusicPlayback";

MusicPlaybackService& MusicPlaybackService::getInstance() {
    static MusicPlaybackService instance;
    return instance;
}

MusicPlaybackService::MusicPlaybackService() {}

bool MusicPlaybackService::begin() {
    if (_initialized) return true;

    CatalogDB::getInstance().begin();
    
    m_cmd_queue = xQueueCreate(10, sizeof(MediaCommand));
    if (!m_cmd_queue) {
        ESP_LOGE(TAG, "Failed to create media command queue");
        return false;
    }

    m_aux_queue = xQueueCreate(4, sizeof(MediaAuxCommand));
    if (!m_aux_queue) {
        ESP_LOGE(TAG, "Failed to create media aux queue");
        vQueueDelete(m_cmd_queue);
        m_cmd_queue = nullptr;
        return false;
    }

    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        workerTask,
        "media_worker",
        ThreadConfig::StackSize::STACK_PLAYER,
        this,
        ThreadConfig::Priority::NORMAL,
        &m_worker_task,
        ThreadConfig::CORE_NETWORK,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (ret != pdPASS) {
        ret = xTaskCreatePinnedToCore(
            workerTask,
            "media_worker",
            ThreadConfig::StackSize::STACK_PLAYER,
            this,
            ThreadConfig::Priority::NORMAL,
            &m_worker_task,
            ThreadConfig::CORE_NETWORK
        );
    }
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn media_worker task");
        return false;
    }

    ret = xTaskCreatePinnedToCoreWithCaps(
        auxWorkerTask,
        "media_aux",
        ThreadConfig::StackSize::STACK_PLAYER,
        this,
        ThreadConfig::Priority::LOW,
        &m_aux_task,
        ThreadConfig::CORE_ANY,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (ret != pdPASS) {
        ret = xTaskCreatePinnedToCore(
            auxWorkerTask,
            "media_aux",
            ThreadConfig::StackSize::STACK_PLAYER,
            this,
            ThreadConfig::Priority::LOW,
            &m_aux_task,
            ThreadConfig::CORE_ANY
        );
    }
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn media_aux task");
        return false;
    }

    NexusPlayer::getInstance().addObserver(this);
    _autoplayEnabled = EmbeddedSysDb::getInstance().snapshot().media.autoplay_enabled;
    _initialized = true;
    ESP_LOGI(TAG, "MusicPlaybackService initialized with persistent worker queues (autoplay=%s, caching=%s)",
             _autoplayEnabled ? "true" : "false",
             EmbeddedSysDb::getInstance().snapshot().media.cache_downloads ? "true" : "false");
    return true;
}

void MusicPlaybackService::workerTask(void* arg) {
    auto* self = static_cast<MusicPlaybackService*>(arg);
    self->workerLoop();
}

void MusicPlaybackService::auxWorkerTask(void* arg) {
    auto* self = static_cast<MusicPlaybackService*>(arg);
    self->auxWorkerLoop();
}

bool MusicPlaybackService::postCommand(MediaCmdType type, const char* query) {
    if (!_initialized) return false;

    // Fast-path low-latency controls execute directly without queue delay
    if (type == MediaCmdType::PAUSE) {
        NexusPlayer::getInstance().pause();
        return true;
    }
    if (type == MediaCmdType::RESUME) {
        NexusPlayer::getInstance().resume();
        return true;
    }
    if (type == MediaCmdType::TOGGLE_PLAY_PAUSE) {
        auto st = NexusPlayer::getInstance().getState();
        if (st == STATE_PAUSED) {
            NexusPlayer::getInstance().resume();
        } else if (st == STATE_STREAMING_AND_CACHING || st == STATE_LOCAL_PLAYBACK) {
            NexusPlayer::getInstance().pause();
        }
        return true;
    }
    if (type == MediaCmdType::STOP) {
        NexusPlayer::getInstance().stop();
        invalidateBackgroundWork();
        clearQueue();
        {
            std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
            _currentTrack = InvidiousTrack();
        }
        if (m_cmd_queue) {
            MediaCommand dummy;
            while (xQueueReceive(m_cmd_queue, &dummy, 0) == pdTRUE) {}
        }
        EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
            s.media.state = MediaPlaybackState::IDLE;
            s.media.active_song_id[0] = '\0';
            s.media.title[0] = '\0';
            s.media.artist[0] = '\0';
            s.media.position_ms = 0;
            s.media.duration_ms = 0;
            s.media.seekable = false;
        });
        return true;
    }

    if (type == MediaCmdType::SEEK) {
        MediaCommand cmd{};
        cmd.type = type;
        {
            std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
            cmd.generation = _queueGeneration;
        }
        if (query) {
            strncpy(cmd.query, query, sizeof(cmd.query) - 1);
            cmd.query[sizeof(cmd.query) - 1] = '\0';
        }
        if (m_cmd_queue) {
            return xQueueSend(m_cmd_queue, &cmd, 0) == pdTRUE;
        }
        return false;
    }

    if (type == MediaCmdType::PLAY) {
        // High-level stop and invalidate existing work so the new track starts fresh
        NexusPlayer::getInstance().stop();
        invalidateBackgroundWork();
        if (m_cmd_queue) {
            MediaCommand dummy;
            while (xQueueReceive(m_cmd_queue, &dummy, 0) == pdTRUE) {}
        }
        EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
            s.media.state = MediaPlaybackState::RESOLVING;
        });
    }

    MediaCommand cmd{};
    cmd.type = type;
    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        cmd.generation = _queueGeneration;
    }
    if (query) {
        strncpy(cmd.query, query, sizeof(cmd.query) - 1);
        cmd.query[sizeof(cmd.query) - 1] = '\0';
    }

    if (xQueueSend(m_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to post media command %d (queue full)", static_cast<int>(type));
        return false;
    }
    return true;
}

void MusicPlaybackService::workerLoop() {
    ESP_LOGI(TAG, "Persistent media_worker started");
    MediaCommand cmd;
    while (true) {
        if (xQueueReceive(m_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            {
                std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
                if (cmd.type != MediaCmdType::PLAY && cmd.generation != _queueGeneration) {
                    ESP_LOGD(TAG, "Discarding stale media command %d (gen %lu != %lu)",
                             static_cast<int>(cmd.type),
                             (unsigned long)cmd.generation,
                             (unsigned long)_queueGeneration);
                    continue;
                }
            }

            switch (cmd.type) {
                case MediaCmdType::PLAY:
                    clearQueue();
                    resolveAndPlayImmediate(cmd.query);
                    break;
                case MediaCmdType::PLAY_NEXT:
                    playNextInternal(cmd.query);
                    break;
                case MediaCmdType::QUEUE:
                    queueInternal(cmd.query);
                    break;
                case MediaCmdType::NEXT:
                    nextInternal();
                    break;
                case MediaCmdType::PREVIOUS:
                    previousInternal();
                    break;
                case MediaCmdType::REPLAY: {
                    InvidiousTrack curr;
                    {
                        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
                        curr = _currentTrack;
                    }
                    if (!curr.videoId.empty()) {
                        playTrack(curr);
                    }
                    break;
                }
                case MediaCmdType::SEEK: {
                    uint32_t ms = static_cast<uint32_t>(strtoul(cmd.query, nullptr, 10));
                    ESP_LOGI(TAG, "Executing SEEK command to %u ms", (unsigned int)ms);
                    NexusPlayer::getInstance().seekTo(ms);
                    break;
                }
                default:
                    break;
            }
        }
    }
}

void MusicPlaybackService::auxWorkerLoop() {
    ESP_LOGI(TAG, "Persistent media_aux started");
    MediaAuxCommand cmd;
    while (true) {
        if (xQueueReceive(m_aux_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            {
                std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
                if (cmd.generation != _queueGeneration) {
                    ESP_LOGD(TAG, "Aux command discarded (gen %lu != %lu)",
                             (unsigned long)cmd.generation, (unsigned long)_queueGeneration);
                    if (cmd.type == MediaAuxCmdType::PREFETCH) _prefetchInProgress = false;
                    if (cmd.type == MediaAuxCmdType::REPLENISH) _replenishInProgress = false;
                    continue;
                }
            }

            if (cmd.type == MediaAuxCmdType::PREFETCH) {
                handlePrefetch(cmd.targetId, cmd.generation);
            } else if (cmd.type == MediaAuxCmdType::REPLENISH) {
                handleReplenish(cmd.targetId, cmd.baseAuthor, cmd.baseTitle, cmd.generation);
            } else if (cmd.type == MediaAuxCmdType::FETCH_THUMBNAIL) {
                char thumbPath[128];
                snprintf(thumbPath, sizeof(thumbPath), "%s/%s.jpg", CatalogDB::THUMBS_DIR, cmd.targetId);
                struct stat st;
                if (stat(thumbPath, &st) != 0 || st.st_size == 0) {
                    std::string thumbUrl = "https://i.ytimg.com/vi/" + std::string(cmd.targetId) + "/default.jpg";
                    ESP_LOGI(TAG, "Fetching album art thumbnail: %s", thumbUrl.c_str());

                    esp_http_client_config_t config = {};
                    config.url = thumbUrl.c_str();
                    config.timeout_ms = 8000;
                    config.skip_cert_common_name_check = true;
                    esp_http_client_handle_t client = esp_http_client_init(&config);
                    if (client) {
                        esp_err_t err = esp_http_client_open(client, 0);
                        if (err == ESP_OK) {
                            int64_t clen = esp_http_client_fetch_headers(client);
                            if (clen > 0 && clen < 65536) {
                                FILE* tf = fopen(thumbPath, "wb");
                                if (tf) {
                                    char tbuf[1024];
                                    int r = 0;
                                    while ((r = esp_http_client_read(client, tbuf, sizeof(tbuf))) > 0) {
                                        fwrite(tbuf, 1, r, tf);
                                    }
                                    fclose(tf);
                                    CatalogDB::getInstance().setThumbnailCached(cmd.targetId, true);
                                    ESP_LOGI(TAG, "Saved album art thumbnail to %s", thumbPath);
                                }
                            }
                        }
                        esp_http_client_cleanup(client);
                    }
                }
            }
        }
    }
}

void MusicPlaybackService::setAutoplay(bool enabled) {
    _autoplayEnabled = enabled;
    EmbeddedSysDb::getInstance().mutate([enabled](SystemState& s) {
        s.media.autoplay_enabled = enabled;
    });
    ESP_LOGI(TAG, "Autoplay set to %s (persisted to SysDb)", enabled ? "true" : "false");
}

bool MusicPlaybackService::isAutoplayEnabled() const {
    return EmbeddedSysDb::getInstance().snapshot().media.autoplay_enabled;
}

void MusicPlaybackService::setCaching(bool enabled) {
    EmbeddedSysDb::getInstance().mutate([enabled](SystemState& s) {
        s.media.cache_downloads = enabled;
    });
    ESP_LOGI(TAG, "Live caching set to %s (persisted to SysDb)", enabled ? "true" : "false");
}

bool MusicPlaybackService::isCachingEnabled() const {
    return EmbeddedSysDb::getInstance().snapshot().media.cache_downloads;
}

void MusicPlaybackService::setRepeatMode(RepeatMode mode) {
    _repeatMode = mode;
    EmbeddedSysDb::getInstance().mutate([mode](SystemState& s) {
        s.media.repeat_mode = static_cast<uint8_t>(mode);
    });
}

bool MusicPlaybackService::isTrackInQueueOrHistory(const std::string& videoId) const {
    if (videoId.empty()) return true;
    if (_currentTrack.videoId == videoId) return true;
    for (const auto& t : _queue) {
        if (t.videoId == videoId) return true;
    }
    for (const auto& t : _history) {
        if (t.videoId == videoId) return true;
    }
    return false;
}

void MusicPlaybackService::populateRecommendations(const std::vector<InvidiousTrack>& recs, const std::string& title) {
    if (recs.empty()) return;
    std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
    size_t added = 0;
    for (const auto& r : recs) {
        if (!isTrackInQueueOrHistory(r.videoId)) {
            _queue.push_back(r);
            added++;
        }
    }
    ESP_LOGI(TAG, "Populated queue with %zu recommended tracks (total queue: %zu) for '%s'",
             added, _queue.size(), title.c_str());
    prefetchNextTrack();
}

void MusicPlaybackService::invalidateBackgroundWork() {
    std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
    _queueGeneration++;
    _prefetchInProgress = false;
    _replenishInProgress = false;
    if (m_aux_queue) {
        MediaAuxCommand dummy;
        while (xQueueReceive(m_aux_queue, &dummy, 0) == pdTRUE) {}
    }
}

bool MusicPlaybackService::playTrack(const InvidiousTrack& track) {
    return playTrackInternal(track) == ESP_OK;
}

esp_err_t MusicPlaybackService::playTrackInternal(const InvidiousTrack& track) {
    if (track.videoId.empty()) return ESP_ERR_INVALID_ARG;

    // Check if the track is already cached locally on the SD card
    if (NexusPlayer::getInstance().getStorageManager().fileExists(track.videoId.c_str())) {
        ESP_LOGI(TAG, "Track '%s' [%s] found in local cache! Playing immediately (0ms network delay)",
                 track.title.c_str(), track.videoId.c_str());
        {
            std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
            _currentTrack = track;
        }
        EmbeddedSysDb::getInstance().mutate([&track](SystemState& s) {
            s.media.state = MediaPlaybackState::PLAYING;
            strncpy(s.media.active_song_id, track.videoId.c_str(), sizeof(s.media.active_song_id) - 1);
            s.media.active_song_id[sizeof(s.media.active_song_id) - 1] = '\0';
            strncpy(s.media.title, track.title.c_str(), sizeof(s.media.title) - 1);
            s.media.title[sizeof(s.media.title) - 1] = '\0';
            strncpy(s.media.artist, track.author.c_str(), sizeof(s.media.artist) - 1);
            s.media.artist[sizeof(s.media.artist) - 1] = '\0';
        });
        NexusPlayer::getInstance().play(track.videoId.c_str(), "");
        return ESP_OK;
    }

    std::string streamUrl;
    std::vector<InvidiousTrack> recommendations;

    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        if (_prefetchedVideoId == track.videoId && !_prefetchedUrl.empty()) {
            ESP_LOGI(TAG, "Using pre-fetched stream URL for '%s' (0ms network delay!)", track.videoId.c_str());
            streamUrl = _prefetchedUrl;
            _prefetchedUrl.clear();
            _prefetchedVideoId.clear();
        }
    }

    // If prefetch is in progress for this track, wait briefly for it to complete
    if (streamUrl.empty() && _prefetchInProgress) {
        ESP_LOGI(TAG, "Prefetch in progress for '%s'; waiting for background task...", track.videoId.c_str());
        int waitMs = 0;
        while (_prefetchInProgress && waitMs < 3500) {
            vTaskDelay(pdMS_TO_TICKS(50));
            waitMs += 50;
        }
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        if (_prefetchedVideoId == track.videoId && !_prefetchedUrl.empty()) {
            ESP_LOGI(TAG, "Using pre-fetched stream URL for '%s' after %d ms wait", track.videoId.c_str(), waitMs);
            streamUrl = _prefetchedUrl;
            _prefetchedUrl.clear();
            _prefetchedVideoId.clear();
        }
    }

    if (streamUrl.empty()) {
        // Stop current playback immediately to clear network streaming, decoding, and Core 0 load
        NexusPlayer::getInstance().stop();
        ESP_LOGI(TAG, "Resolving WebM/Opus stream for '%s' [%s]...", track.title.c_str(), track.videoId.c_str());

        // Resolve stream URL and piggyback recommendation fetch in a single HTTP roundtrip
        esp_err_t err = _invidious.resolveWithRecommendations(track.videoId, streamUrl, recommendations, 8);
        if ((err != ESP_OK && err != ESP_ERR_NOT_FOUND) || (err == ESP_OK && streamUrl.empty())) {
            ESP_LOGW(TAG, "resolveWithRecommendations failed for '%s' (%s), attempting standalone WebM/Opus resolve...",
                     track.title.c_str(), esp_err_to_name(err));
            err = _invidious.resolveWebMOpusStreamUrl(track.videoId, streamUrl);
        }
        if (err != ESP_OK || streamUrl.empty()) {
            bool gone = (err == ESP_ERR_NOT_FOUND);
            ESP_LOGW(TAG, "%s stream for '%s' [%s] (%s)",
                     gone ? "Unavailable" : "Failed to resolve",
                     track.title.c_str(), track.videoId.c_str(), esp_err_to_name(err));
            return gone ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Playing: '%s' by '%s' [%s]",
             track.title.c_str(), track.author.c_str(), track.videoId.c_str());

    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        _currentTrack = track;
    }
    EmbeddedSysDb::getInstance().mutate([&track](SystemState& s) {
        s.media.state = MediaPlaybackState::PLAYING;
        strncpy(s.media.active_song_id, track.videoId.c_str(), sizeof(s.media.active_song_id) - 1);
        s.media.active_song_id[sizeof(s.media.active_song_id) - 1] = '\0';
        strncpy(s.media.title, track.title.c_str(), sizeof(s.media.title) - 1);
        s.media.title[sizeof(s.media.title) - 1] = '\0';
        strncpy(s.media.artist, track.author.c_str(), sizeof(s.media.artist) - 1);
        s.media.artist[sizeof(s.media.artist) - 1] = '\0';
    });
    NexusPlayer::getInstance().play(track.videoId.c_str(), streamUrl.c_str());

    // If recommendations were piggybacked and queue is empty or low, add them to the queue
    if (isAutoplayEnabled() && !recommendations.empty()) {
        bool shouldPopulate = false;
        {
            std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
            if (_queue.empty() || _queue.size() <= QUEUE_LOW_WATERMARK) {
                shouldPopulate = true;
            }
        }
        if (shouldPopulate) {
            populateRecommendations(recommendations, track.title);
        }
    }

    return ESP_OK;
}

bool MusicPlaybackService::playTrackFallback(const InvidiousTrack& track) {
    if (track.videoId.empty()) return false;

    std::string streamUrl;
    esp_err_t err = _invidious.resolveWebMOpusStreamUrl(track.videoId, streamUrl);
    if (err != ESP_OK || streamUrl.empty()) {
        ESP_LOGE(TAG, "Fallback failed to resolve Opus stream for '%s'", track.title.c_str());
        return false;
    }

    ESP_LOGI(TAG, "Fallback playing live stream: '%s' by '%s' [%s]",
             track.title.c_str(), track.author.c_str(), track.videoId.c_str());

    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        _currentTrack = track;
    }
    EmbeddedSysDb::getInstance().mutate([&track](SystemState& s) {
        s.media.state = MediaPlaybackState::PLAYING;
        strncpy(s.media.active_song_id, track.videoId.c_str(), sizeof(s.media.active_song_id) - 1);
        s.media.active_song_id[sizeof(s.media.active_song_id) - 1] = '\0';
        strncpy(s.media.title, track.title.c_str(), sizeof(s.media.title) - 1);
        s.media.title[sizeof(s.media.title) - 1] = '\0';
        strncpy(s.media.artist, track.author.c_str(), sizeof(s.media.artist) - 1);
        s.media.artist[sizeof(s.media.artist) - 1] = '\0';
    });
    NexusPlayer::getInstance().play(track.videoId.c_str(), streamUrl.c_str());
    return true;
}

bool MusicPlaybackService::resolveAndPlayImmediate(const char* query) {
    if (!query || query[0] == '\0') {
        ESP_LOGE(TAG, "Empty music query");
        return false;
    }
    ESP_LOGI(TAG, "resolveAndPlayImmediate: resolving track for '%s'...", query);

    NexusPlayer::getInstance().stop();

    InvidiousTrack track;
    esp_err_t err = _invidious.search(query, track);
    if (err != ESP_OK || track.videoId.empty()) {
        ESP_LOGE(TAG, "Search failed for '%s': %s", query, esp_err_to_name(err));
        EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
            s.media.state = MediaPlaybackState::IDLE;
        });
        return false;
    }

    clearQueue();
    ESP_LOGI(TAG, "Search for '%s' resolved to: '%s' by '%s' [%s]",
             query, track.title.c_str(), track.author.c_str(), track.videoId.c_str());

    return playTrack(track);
}

bool MusicPlaybackService::playNextInternal(const char* query) {
    if (!query || query[0] == '\0') return false;

    InvidiousTrack track;
    esp_err_t err = _invidious.search(query, track);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Search failed for play_next '%s'", query);
        return false;
    }

    if (NexusPlayer::getInstance().getState() == STATE_IDLE) {
        return playTrack(track);
    } else {
        {
            std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
            _queue.push_front(track);
            _prefetchedUrl.clear();
            _prefetchedVideoId.clear();
            ESP_LOGI(TAG, "Queued to play next: '%s' (queue depth: %zu)", track.title.c_str(), _queue.size());
        }
        prefetchNextTrack();
        return true;
    }
}

bool MusicPlaybackService::queueInternal(const char* query) {
    if (!query || query[0] == '\0') return false;

    InvidiousTrack track;
    esp_err_t err = _invidious.search(query, track);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Search failed for queue '%s'", query);
        return false;
    }

    if (NexusPlayer::getInstance().getState() == STATE_IDLE) {
        return playTrack(track);
    } else {
        bool shouldPrefetch = false;
        {
            std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
            _queue.push_back(track);
            ESP_LOGI(TAG, "Queued track: '%s' (queue depth: %zu)", track.title.c_str(), _queue.size());
            if (_queue.size() == 1 && _prefetchedUrl.empty()) {
                shouldPrefetch = true;
            }
        }
        if (shouldPrefetch) {
            prefetchNextTrack();
        }
        return true;
    }
}

void MusicPlaybackService::clearQueue() {
    std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
    _queue.clear();
    _prefetchedUrl.clear();
    _prefetchedVideoId.clear();
    _queueGeneration++;
    ESP_LOGI(TAG, "Playback queue cleared");
}

void MusicPlaybackService::shuffleQueue() {
    std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
    if (_queue.size() <= 1) return;
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(_queue.begin(), _queue.end(), g);
    _prefetchedUrl.clear();
    _prefetchedVideoId.clear();
    ESP_LOGI(TAG, "Playback queue shuffled (%zu tracks)", _queue.size());
    prefetchNextTrack();
}

bool MusicPlaybackService::postAuxCommand(MediaAuxCmdType type, const char* targetId, const char* author, const char* title) {
    if (!m_aux_queue || !targetId || targetId[0] == '\0') return false;
    MediaAuxCommand cmd{};
    cmd.type = type;
    cmd.generation = _queueGeneration;
    strncpy(cmd.targetId, targetId, sizeof(cmd.targetId) - 1);
    if (author) strncpy(cmd.baseAuthor, author, sizeof(cmd.baseAuthor) - 1);
    if (title)  strncpy(cmd.baseTitle,  title,  sizeof(cmd.baseTitle) - 1);
    return (xQueueSend(m_aux_queue, &cmd, 0) == pdTRUE);
}

void MusicPlaybackService::prefetchNextTrack() {
    std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
    if (_queue.empty() || _prefetchInProgress) return;

    std::string nextId = _queue.front().videoId;
    if (nextId.empty() || nextId == _prefetchedVideoId) return;

    if (NexusPlayer::getInstance().getStorageManager().fileExists(nextId.c_str())) {
        ESP_LOGD(TAG, "Next track %s is already cached locally, skipping prefetch", nextId.c_str());
        return;
    }

    _prefetchInProgress = true;
    if (!postAuxCommand(MediaAuxCmdType::PREFETCH, nextId.c_str())) {
        _prefetchInProgress = false;
        ESP_LOGW(TAG, "Aux queue full, skipping prefetch for %s", nextId.c_str());
    }
}

void MusicPlaybackService::handlePrefetch(const char* targetId, uint32_t generation) {
    if (!targetId || targetId[0] == '\0') {
        _prefetchInProgress = false;
        return;
    }

    // Yield CPU so playback startup, I2S DMA, and AFE processing settle cleanly
    vTaskDelay(pdMS_TO_TICKS(150));

    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        if (generation != _queueGeneration) {
            ESP_LOGD(TAG, "Prefetch for %s discarded: generation mismatch (%lu vs %lu)",
                     targetId, (unsigned long)generation, (unsigned long)_queueGeneration);
            _prefetchInProgress = false;
            return;
        }
    }

    std::string url;
    esp_err_t err = _invidious.resolveWebMOpusStreamUrl(targetId, url);
    if (err == ESP_OK && !url.empty()) {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        if (generation == _queueGeneration) {
            _prefetchedVideoId = targetId;
            _prefetchedUrl = url;
            ESP_LOGI(TAG, "Pre-fetched stream URL for next track: %s", targetId);
        }
    } else {
        ESP_LOGW(TAG, "Pre-fetch failed for %s: %s", targetId, esp_err_to_name(err));
    }
    _prefetchInProgress = false;
}

void MusicPlaybackService::checkAndReplenishQueue() {
    std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
    if (_queue.size() > QUEUE_LOW_WATERMARK || _replenishInProgress) return;

    std::string baseTrackId;
    std::string baseAuthor;
    std::string baseTitle;
    if (!_queue.empty()) {
        baseTrackId = _queue.back().videoId;
        baseAuthor = _queue.back().author;
        baseTitle = _queue.back().title;
    } else if (!_currentTrack.videoId.empty()) {
        baseTrackId = _currentTrack.videoId;
        baseAuthor = _currentTrack.author;
        baseTitle = _currentTrack.title;
    }

    if (baseTrackId.empty()) return;

    _replenishInProgress = true;
    if (!postAuxCommand(MediaAuxCmdType::REPLENISH, baseTrackId.c_str(), baseAuthor.c_str(), baseTitle.c_str())) {
        _replenishInProgress = false;
        ESP_LOGW(TAG, "Aux queue full, skipping replenish for %s", baseTrackId.c_str());
    }
}

void MusicPlaybackService::handleReplenish(const char* baseId, const char* baseAuthor, const char* baseTitle, uint32_t generation) {
    if (!baseId || baseId[0] == '\0') {
        _replenishInProgress = false;
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    auto stale = [&]() {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        return _queueGeneration != generation;
    };

    std::vector<InvidiousTrack> recs;
    esp_err_t err = _invidious.getRecommendedTracks(baseId, recs, 8);
    if ((err != ESP_OK || recs.empty()) && baseAuthor && baseAuthor[0] != '\0' && !stale()) {
        ESP_LOGI(TAG, "Recommended videos not returned for %s (%s). Falling back to search for artist '%s'...",
                 baseId, esp_err_to_name(err), baseAuthor);
        err = _invidious.searchList(baseAuthor, recs, 8);
    }
    if ((err != ESP_OK || recs.empty()) && baseTitle && baseTitle[0] != '\0' && !stale()) {
        ESP_LOGI(TAG, "Falling back to search for title '%s'...", baseTitle);
        err = _invidious.searchList(baseTitle, recs, 8);
    }

    bool needPrefetch = false;
    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        if (_queueGeneration == generation) {
            if (err == ESP_OK && !recs.empty()) {
                size_t added = 0;
                for (const auto& track : recs) {
                    if (!isTrackInQueueOrHistory(track.videoId)) {
                        _queue.push_back(track);
                        added++;
                    }
                }
                ESP_LOGI(TAG, "Autoplay replenished %zu new recommendations (queue size now %zu) based on %s",
                         added, _queue.size(), baseId);

                if (_prefetchedUrl.empty()) {
                    needPrefetch = true;
                }
            } else {
                ESP_LOGW(TAG, "Failed to replenish recommendations for %s: %s",
                         baseId, esp_err_to_name(err));
            }
        } else {
            ESP_LOGD(TAG, "Replenish for %s discarded (generation changed)", baseId);
        }
        _replenishInProgress = false;
    }

    if (needPrefetch) {
        prefetchNextTrack();
    }
}

bool MusicPlaybackService::play(const char* query) {
    return postCommand(MediaCmdType::PLAY, query);
}

bool MusicPlaybackService::playDirect(const InvidiousTrack& track, const char* streamUrl) {
    if (!streamUrl || streamUrl[0] == '\0') return false;

    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        _queueGeneration++;
        _currentTrack = track;
        if (track.videoId.length() > 0 && (_history.empty() || _history.back().videoId != track.videoId)) {
            _history.push_back(track);
            if (_history.size() > 20) _history.erase(_history.begin());
        }
    }

    // Index into local SD library
    auto rec = std::make_unique<TrackRecord>();
    strncpy(rec->videoId, track.videoId.c_str(), sizeof(rec->videoId) - 1);
    strncpy(rec->title, track.title.c_str(), sizeof(rec->title) - 1);
    strncpy(rec->artist, track.author.c_str(), sizeof(rec->artist) - 1);
    rec->durationMs = track.durationSeconds * 1000;
    rec->cachedAt = static_cast<uint32_t>(time(nullptr));
    rec->sampleRate = 48000;
    rec->channels = 2;
    rec->codecId = 0; // WebM/Opus
    CatalogDB::getInstance().upsert(*rec);
    CatalogDB::getInstance().recordPlay(track.videoId.c_str());

    // Dispatch thumbnail prefetch to background aux task
    postAuxCommand(MediaAuxCmdType::FETCH_THUMBNAIL, track.videoId.c_str(), track.author.c_str(), track.title.c_str());

    // Call NexusPlayer directly with direct stream URL
    NexusPlayer::getInstance().play(track.videoId.c_str(), streamUrl);

    EmbeddedSysDb::getInstance().mutate([&track](SystemState& s) {
        s.media.state = MediaPlaybackState::PLAYING;
        strncpy(s.media.active_song_id, track.videoId.c_str(), sizeof(s.media.active_song_id) - 1);
        s.media.active_song_id[sizeof(s.media.active_song_id) - 1] = '\0';
        strncpy(s.media.title, track.title.c_str(), sizeof(s.media.title) - 1);
        strncpy(s.media.artist, track.author.c_str(), sizeof(s.media.artist) - 1);
        s.media.duration_ms = track.durationSeconds * 1000;
        s.media.position_ms = 0;
        s.media.seekable = true;
    });

    ESP_LOGI(TAG, "playDirect: Started '%s' (%s)", track.title.c_str(), track.videoId.c_str());
    return true;
}

bool MusicPlaybackService::playLocal(const char* songIdOrPath) {
    if (!songIdOrPath || songIdOrPath[0] == '\0') return false;

    std::string id = songIdOrPath;
    auto rec = std::make_unique<TrackRecord>();
    if (!CatalogDB::getInstance().get(id.c_str(), *rec)) {
        // Fallback: sync filesystem and search again
        CatalogDB::getInstance().scanAndSync();
        if (!CatalogDB::getInstance().get(id.c_str(), *rec)) {
            ESP_LOGW(TAG, "playLocal: Track %s not found in CatalogDB", id.c_str());
            return false;
        }
    }

    InvidiousTrack track;
    track.videoId = rec->videoId;
    track.title = rec->title;
    track.author = rec->artist;
    track.durationSeconds = rec->durationMs / 1000;

    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        _queueGeneration++;
        _currentTrack = track;
        if (_history.empty() || _history.back().videoId != track.videoId) {
            _history.push_back(track);
            if (_history.size() > 20) _history.erase(_history.begin());
        }
    }

    CatalogDB::getInstance().recordPlay(rec->videoId);
    NexusPlayer::getInstance().play(rec->videoId, "");

    EmbeddedSysDb::getInstance().mutate([&track](SystemState& s) {
        s.media.state = MediaPlaybackState::PLAYING;
        strncpy(s.media.active_song_id, track.videoId.c_str(), sizeof(s.media.active_song_id) - 1);
        s.media.active_song_id[sizeof(s.media.active_song_id) - 1] = '\0';
        strncpy(s.media.title, track.title.c_str(), sizeof(s.media.title) - 1);
        strncpy(s.media.artist, track.author.c_str(), sizeof(s.media.artist) - 1);
        s.media.duration_ms = track.durationSeconds * 1000;
        s.media.position_ms = 0;
        s.media.seekable = true;
    });

    ESP_LOGI(TAG, "playLocal: Playing '%s' (%s)", track.title.c_str(), track.videoId.c_str());
    return true;
}

bool MusicPlaybackService::playNext(const char* query) {
    return postCommand(MediaCmdType::PLAY_NEXT, query);
}

bool MusicPlaybackService::queue(const char* query) {
    return postCommand(MediaCmdType::QUEUE, query);
}

bool MusicPlaybackService::next() {
    return postCommand(MediaCmdType::NEXT);
}

bool MusicPlaybackService::nextInternal() {
    ESP_LOGI(TAG, "Advancing to next track");
    invalidateBackgroundWork();

    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        if (!_currentTrack.videoId.empty() && (_history.empty() || _history.back().videoId != _currentTrack.videoId)) {
            _history.push_back(_currentTrack);
            if (_history.size() > 20) {
                _history.erase(_history.begin());
            }
        }
    }

    return advanceToNextPlayable();
}

bool MusicPlaybackService::advanceToNextPlayable() {
    for (int skips = 0; skips < MAX_SKIP_ON_ADVANCE; ++skips) {
        InvidiousTrack nextTrack;
        {
            std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
            if (!_queue.empty()) {
                nextTrack = _queue.front();
                _queue.pop_front();
            }
        }
        if (nextTrack.videoId.empty()) break;

        esp_err_t rc = playTrackInternal(nextTrack);
        if (rc == ESP_OK) return true;
        if (rc == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Track '%s' [%s] unavailable - skipping to next queued track",
                     nextTrack.title.c_str(), nextTrack.videoId.c_str());
            continue;
        }
        ESP_LOGE(TAG, "Transient error advancing to '%s'; halting playback", nextTrack.title.c_str());
        NexusPlayer::getInstance().stop();
        return false;
    }

    if (isAutoplayEnabled()) {
        std::string currentId;
        {
            std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
            currentId = _currentTrack.videoId;
        }

        if (!currentId.empty()) {
            ESP_LOGI(TAG, "Queue empty; attempting emergency autoplay recommendations for %s", currentId.c_str());
            std::vector<InvidiousTrack> recTracks;
            esp_err_t err = _invidious.getRecommendedTracks(currentId, recTracks, 8);
            if (err == ESP_OK && !recTracks.empty()) {
                {
                    std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
                    for (const auto& t : recTracks) {
                        if (!isTrackInQueueOrHistory(t.videoId)) {
                            _queue.push_back(t);
                        }
                    }
                }
                for (int skips = 0; skips < MAX_SKIP_ON_ADVANCE; ++skips) {
                    InvidiousTrack cand;
                    {
                        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
                        if (_queue.empty()) break;
                        cand = _queue.front();
                        _queue.pop_front();
                    }
                    esp_err_t rc = playTrackInternal(cand);
                    if (rc == ESP_OK) {
                        ESP_LOGI(TAG, "Autoplay emergency playing: '%s'", cand.title.c_str());
                        return true;
                    }
                    if (rc != ESP_ERR_NOT_FOUND) break;
                    ESP_LOGW(TAG, "Autoplay candidate '%s' unavailable - skipping", cand.videoId.c_str());
                }
            }
        }
    }

    ESP_LOGI(TAG, "No more playable tracks in queue or recommendations");
    NexusPlayer::getInstance().stop();
    EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
        s.media.state = MediaPlaybackState::IDLE;
        s.media.active_song_id[0] = '\0';
        s.media.title[0] = '\0';
        s.media.artist[0] = '\0';
    });
    return false;
}

bool MusicPlaybackService::previous() {
    return postCommand(MediaCmdType::PREVIOUS);
}

bool MusicPlaybackService::previousInternal() {
    ESP_LOGI(TAG, "Going back to previous track");
    invalidateBackgroundWork();

    for (int skips = 0; skips < MAX_SKIP_ON_ADVANCE; ++skips) {
        InvidiousTrack prevTrack;
        {
            std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
            if (_history.empty()) {
                ESP_LOGW(TAG, "No track history available");
                return false;
            }
            prevTrack = _history.back();
            _history.pop_back();

            if (skips == 0 && !_currentTrack.videoId.empty()) {
                _queue.push_front(_currentTrack);
            }
        }

        esp_err_t rc = playTrackInternal(prevTrack);
        if (rc == ESP_OK) return true;
        if (rc == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Previous track '%s' [%s] unavailable - going further back",
                     prevTrack.title.c_str(), prevTrack.videoId.c_str());
            continue;
        }
        NexusPlayer::getInstance().stop();
        return false;
    }
    return false;
}

void MusicPlaybackService::pause() {
    postCommand(MediaCmdType::PAUSE);
}

void MusicPlaybackService::resume() {
    postCommand(MediaCmdType::RESUME);
}

void MusicPlaybackService::stop() {
    postCommand(MediaCmdType::STOP);
}

void MusicPlaybackService::onTrackStarted(const char* songId) {
    ESP_LOGI(TAG, "Observer event: Track started [%s]", songId ? songId : "");
    if (isAutoplayEnabled()) {
        checkAndReplenishQueue();
    }
    prefetchNextTrack();
}

void MusicPlaybackService::onTrackFinished(const char* songId) {
    ESP_LOGI(TAG, "Observer event: Track finished [%s]", songId ? songId : "");
    InvidiousTrack curr;
    RepeatMode mode;
    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        curr = _currentTrack;
        mode = _repeatMode;
    }

    if (mode == RepeatMode::One && !curr.videoId.empty()) {
        ESP_LOGI(TAG, "Repeating single track: %s", curr.title.c_str());
        postCommand(MediaCmdType::REPLAY);
        return;
    }

    if (!curr.videoId.empty()) {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        if (mode == RepeatMode::All) {
            _queue.push_back(curr);
        }
        _history.push_back(curr);
        if (_history.size() > 20) {
            _history.erase(_history.begin());
        }
    }
    postCommand(MediaCmdType::NEXT);
}

void MusicPlaybackService::onPlaybackError(const char* songId, int errorCode) {
    ESP_LOGE(TAG, "Observer event: Playback error %d for [%s]", errorCode, songId ? songId : "");
    InvidiousTrack curr;
    bool hasQueue = false;
    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        curr = _currentTrack;
        hasQueue = !_queue.empty();
    }

    if (errorCode == -1 && songId && curr.videoId == songId) {
        ESP_LOGW(TAG, "Local file error for %s. Deleting corrupted cache and falling back to live stream!", songId);
        NexusPlayer::getInstance().getStorageManager().deleteFile(songId);
        postCommand(MediaCmdType::REPLAY);
        return;
    }
    if (hasQueue || isAutoplayEnabled()) {
        postCommand(MediaCmdType::NEXT);
    } else {
        EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
            s.media.state = MediaPlaybackState::ERROR_STATE;
        });
    }
}

bool MusicPlaybackService::seekTo(uint32_t positionMs) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", (unsigned int)positionMs);
    return postCommand(MediaCmdType::SEEK, buf);
}

uint32_t MusicPlaybackService::getPositionMs() const {
    return NexusPlayer::getInstance().getPositionMs();
}

