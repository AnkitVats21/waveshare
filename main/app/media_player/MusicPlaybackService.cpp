#include "MusicPlaybackService.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "common/thread_config.h"
#include <algorithm>
#include <random>

static const char* TAG = "MusicPlayback";

MusicPlaybackService& MusicPlaybackService::getInstance() {
    static MusicPlaybackService instance;
    return instance;
}

MusicPlaybackService::MusicPlaybackService() {}

bool MusicPlaybackService::begin() {
    if (_initialized) return true;
    
    NexusPlayer::getInstance().addObserver(this);
    _autoplayEnabled = EmbeddedSysDb::getInstance().snapshot().audio.autoplay_enabled;
    _initialized = true;
    ESP_LOGI(TAG, "MusicPlaybackService initialized (autoplay=%s, caching=%s)",
             _autoplayEnabled ? "true" : "false",
             EmbeddedSysDb::getInstance().snapshot().audio.cache_downloads ? "true" : "false");
    return true;
}

void MusicPlaybackService::setAutoplay(bool enabled) {
    _autoplayEnabled = enabled;
    EmbeddedSysDb::getInstance().mutate([enabled](SystemState& s) {
        s.audio.autoplay_enabled = enabled;
    });
    ESP_LOGI(TAG, "Autoplay set to %s (persisted to SysDb)", enabled ? "true" : "false");
}

bool MusicPlaybackService::isAutoplayEnabled() const {
    return EmbeddedSysDb::getInstance().snapshot().audio.autoplay_enabled;
}

void MusicPlaybackService::setCaching(bool enabled) {
    EmbeddedSysDb::getInstance().mutate([enabled](SystemState& s) {
        s.audio.cache_downloads = enabled;
    });
    ESP_LOGI(TAG, "Live caching set to %s (persisted to SysDb)", enabled ? "true" : "false");
}

bool MusicPlaybackService::isCachingEnabled() const {
    return EmbeddedSysDb::getInstance().snapshot().audio.cache_downloads;
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

bool MusicPlaybackService::playTrack(const InvidiousTrack& track) {
    if (track.videoId.empty()) return false;

    // Check if the track is already cached locally on the SD card
    if (NexusPlayer::getInstance().getStorageManager().fileExists(track.videoId.c_str())) {
        ESP_LOGI(TAG, "Track '%s' [%s] found in local cache! Playing immediately (0ms network delay)",
                 track.title.c_str(), track.videoId.c_str());
        {
            std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
            _currentTrack = track;
        }
        NexusPlayer::getInstance().play(track.videoId.c_str(), "");
        return true;
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

    if (streamUrl.empty()) {
        // Resolve stream URL and piggyback recommendation fetch in a single HTTP roundtrip
        esp_err_t err = _invidious.resolveWithRecommendations(track.videoId, streamUrl, recommendations, 8);
        if (err != ESP_OK || streamUrl.empty()) {
            ESP_LOGE(TAG, "Failed to resolve stream for '%s' (%s): %s",
                     track.title.c_str(), track.videoId.c_str(), esp_err_to_name(err));
            return false;
        }
    }

    ESP_LOGI(TAG, "Playing: '%s' by '%s' [%s]",
             track.title.c_str(), track.author.c_str(), track.videoId.c_str());

    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        _currentTrack = track;
    }
    NexusPlayer::getInstance().play(track.videoId.c_str(), streamUrl.c_str());

    // If recommendations were piggybacked, add them to the queue
    if (isAutoplayEnabled() && !recommendations.empty()) {
        populateRecommendations(recommendations, track.title);
    }

    return true;
}

bool MusicPlaybackService::playTrackFallback(const InvidiousTrack& track) {
    if (track.videoId.empty()) return false;

    std::string streamUrl;
    esp_err_t err = _invidious.resolveOpusUrl(track.videoId, streamUrl);
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
    NexusPlayer::getInstance().play(track.videoId.c_str(), streamUrl.c_str());
    return true;
}

bool MusicPlaybackService::resolveAndPlayImmediate(const char* query) {
    if (!query || query[0] == '\0') {
        ESP_LOGE(TAG, "Empty music query");
        return false;
    }
    ESP_LOGI(TAG, "resolveAndPlayImmediate: resolving track for '%s'...", query);

    // Stop current playback immediately to clear network, decoding, and Core 1 AEC load
    NexusPlayer::getInstance().stop();

    std::vector<InvidiousTrack> tracks;
    esp_err_t err = _invidious.searchList(query, tracks, 1);
    if (err != ESP_OK || tracks.empty()) {
        ESP_LOGE(TAG, "Search failed for '%s': %s", query, esp_err_to_name(err));
        return false;
    }

    clearQueue();
    ESP_LOGI(TAG, "Search for '%s' resolved to: '%s' by '%s' [%s]",
             query, tracks[0].title.c_str(), tracks[0].author.c_str(), tracks[0].videoId.c_str());

    return playTrack(tracks[0]);
}

bool MusicPlaybackService::play(const char* query) {
    ESP_LOGI(TAG, "play() invoked with query: '%s'", query ? query : "(null)");
    clearQueue();
    return resolveAndPlayImmediate(query);
}

bool MusicPlaybackService::playNext(const char* query) {
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
            // Invalidate prefetched since head of queue changed
            _prefetchedUrl.clear();
            _prefetchedVideoId.clear();
            ESP_LOGI(TAG, "Queued to play next: '%s' (queue depth: %zu)", track.title.c_str(), _queue.size());
        }
        prefetchNextTrack();
        return true;
    }
}

bool MusicPlaybackService::queue(const char* query) {
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

void MusicPlaybackService::prefetchNextTrack() {
    std::string nextId;
    uint32_t generation = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        if (_queue.empty() || _prefetchInProgress) return;

        nextId = _queue.front().videoId;
        if (nextId.empty() || nextId == _prefetchedVideoId) return;

        // Skip network prefetch if already cached locally on SD card
        if (NexusPlayer::getInstance().getStorageManager().fileExists(nextId.c_str())) {
            ESP_LOGD(TAG, "Next track %s is already cached locally, skipping prefetch", nextId.c_str());
            return;
        }

        _prefetchInProgress = true;
        generation = _queueGeneration;
    }

    struct PrefetchContext {
        MusicPlaybackService* self;
        std::string targetId;
        uint32_t generation;
        bool withCaps;
    };
    auto* ctx = new PrefetchContext{this, nextId, generation, true};

    auto taskFn = [](void* arg) {
        auto* c = static_cast<PrefetchContext*>(arg);
        MusicPlaybackService* self = c->self;
        std::string targetId = c->targetId;
        uint32_t gen = c->generation;
        bool caps = c->withCaps;
        delete c;

        // Yield CPU so playback startup, I2S DMA, and AFE processing settle cleanly
        vTaskDelay(pdMS_TO_TICKS(150));

        std::string url;
        esp_err_t err = self->_invidious.resolveOpusUrl(targetId, url);

        std::lock_guard<std::recursive_mutex> lock(self->_serviceMutex);
        if (self->_queueGeneration == gen && !self->_queue.empty() && self->_queue.front().videoId == targetId) {
            if (err == ESP_OK && !url.empty()) {
                self->_prefetchedVideoId = targetId;
                self->_prefetchedUrl = url;
                ESP_LOGI(TAG, "Asynchronously pre-fetched stream URL for upcoming track: %s", targetId.c_str());
            } else {
                ESP_LOGW(TAG, "Background prefetch failed for %s: %s", targetId.c_str(), esp_err_to_name(err));
            }
        } else {
            ESP_LOGD(TAG, "Prefetch for %s discarded (queue or generation changed)", targetId.c_str());
        }
        self->_prefetchInProgress = false;
        if (caps) {
            vTaskDeleteWithCaps(NULL);
        } else {
            vTaskDelete(NULL);
        }
    };

    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        taskFn,
        "bg_prefetch",
        ThreadConfig::StackSize::STACK_PLAYER,
        ctx,
        ThreadConfig::Priority::LOW,
        NULL,
        ThreadConfig::CORE_NETWORK,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (ret != pdPASS) {
        ctx->withCaps = false;
        ret = xTaskCreatePinnedToCore(
            taskFn,
            "bg_prefetch",
            ThreadConfig::StackSize::STACK_PLAYER,
            ctx,
            ThreadConfig::Priority::LOW,
            NULL,
            ThreadConfig::CORE_NETWORK
        );
    }

    if (ret != pdPASS) {
        delete ctx;
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        _prefetchInProgress = false;
        ESP_LOGW(TAG, "Failed to spawn background prefetch task");
    }
}

void MusicPlaybackService::checkAndReplenishQueue() {
    std::string baseTrackId;
    uint32_t generation = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        if (!isAutoplayEnabled() || _replenishInProgress) return;
        if (_queue.size() > QUEUE_LOW_WATERMARK) return;

        baseTrackId = !_queue.empty() ? _queue.back().videoId : _currentTrack.videoId;
        if (baseTrackId.empty()) return;

        _replenishInProgress = true;
        generation = _queueGeneration;
    }

    struct ReplenishContext {
        MusicPlaybackService* self;
        std::string baseId;
        uint32_t generation;
        bool withCaps;
    };
    auto* ctx = new ReplenishContext{this, baseTrackId, generation, true};

    auto taskFn = [](void* arg) {
        auto* c = static_cast<ReplenishContext*>(arg);
        MusicPlaybackService* self = c->self;
        std::string baseId = c->baseId;
        uint32_t gen = c->generation;
        bool caps = c->withCaps;
        delete c;

        // Yield CPU to let concurrent audio startup settle
        vTaskDelay(pdMS_TO_TICKS(200));

        std::vector<InvidiousTrack> recs;
        esp_err_t err = self->_invidious.getRecommendedTracks(baseId, recs, 8);

        bool needPrefetch = false;
        {
            std::lock_guard<std::recursive_mutex> lock(self->_serviceMutex);
            if (self->_queueGeneration == gen) {
                if (err == ESP_OK && !recs.empty()) {
                    size_t added = 0;
                    for (const auto& track : recs) {
                        if (!self->isTrackInQueueOrHistory(track.videoId)) {
                            self->_queue.push_back(track);
                            added++;
                        }
                    }
                    ESP_LOGI(TAG, "Autoplay replenished %zu new recommendations (queue size now %zu) based on %s",
                             added, self->_queue.size(), baseId.c_str());

                    if (self->_prefetchedUrl.empty()) {
                        needPrefetch = true;
                    }
                } else {
                    ESP_LOGW(TAG, "Failed to replenish recommendations for %s: %s",
                             baseId.c_str(), esp_err_to_name(err));
                }
            } else {
                ESP_LOGD(TAG, "Replenish for %s discarded (generation changed)", baseId.c_str());
            }
            self->_replenishInProgress = false;
        }

        if (needPrefetch) {
            self->prefetchNextTrack();
        }
        if (caps) {
            vTaskDeleteWithCaps(NULL);
        } else {
            vTaskDelete(NULL);
        }
    };

    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        taskFn,
        "bg_replenish",
        ThreadConfig::StackSize::STACK_PLAYER,
        ctx,
        ThreadConfig::Priority::LOW,
        NULL,
        ThreadConfig::CORE_NETWORK,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (ret != pdPASS) {
        ctx->withCaps = false;
        ret = xTaskCreatePinnedToCore(
            taskFn,
            "bg_replenish",
            ThreadConfig::StackSize::STACK_PLAYER,
            ctx,
            ThreadConfig::Priority::LOW,
            NULL,
            ThreadConfig::CORE_NETWORK
        );
    }

    if (ret != pdPASS) {
        delete ctx;
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        _replenishInProgress = false;
        ESP_LOGW(TAG, "Failed to spawn background replenish task");
    }
}

bool MusicPlaybackService::next() {
    ESP_LOGI(TAG, "Advancing to next track");
    InvidiousTrack nextTrack;
    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        if (!_queue.empty()) {
            nextTrack = _queue.front();
            _queue.pop_front();
        }
    }

    if (!nextTrack.videoId.empty()) {
        return playTrack(nextTrack);
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
                InvidiousTrack first;
                {
                    std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
                    for (const auto& t : recTracks) {
                        if (!isTrackInQueueOrHistory(t.videoId)) {
                            if (first.videoId.empty()) {
                                first = t;
                            } else {
                                _queue.push_back(t);
                            }
                        }
                    }
                }
                if (!first.videoId.empty()) {
                    ESP_LOGI(TAG, "Autoplay emergency populated tracks (playing: '%s')", first.title.c_str());
                    return playTrack(first);
                }
            }
        }
    }

    ESP_LOGI(TAG, "No more tracks in queue or recommendations");
    NexusPlayer::getInstance().stop();
    return false;
}

bool MusicPlaybackService::previous() {
    ESP_LOGI(TAG, "Going back to previous track");
    InvidiousTrack prevTrack;
    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        if (_history.empty()) {
            ESP_LOGW(TAG, "No track history available");
            return false;
        }

        prevTrack = _history.back();
        _history.pop_back();

        if (!_currentTrack.videoId.empty()) {
            _queue.push_front(_currentTrack);
        }
    }

    return playTrack(prevTrack);
}

void MusicPlaybackService::pause() {
    NexusPlayer::getInstance().pause();
}

void MusicPlaybackService::resume() {
    NexusPlayer::getInstance().resume();
}

void MusicPlaybackService::stop() {
    NexusPlayer::getInstance().stop();
    clearQueue();
    std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
    _currentTrack = InvidiousTrack();
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
        playTrack(curr);
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
    next();
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
        if (playTrackFallback(curr)) {
            return;
        }
    }
    if (hasQueue || isAutoplayEnabled()) {
        next();
    }
}

