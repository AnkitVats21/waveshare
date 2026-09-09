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

void MusicPlaybackService::invalidateBackgroundWork() {
    std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
    // Bump the generation so in-flight prefetch/replenish tasks discard their
    // results. An already-completed prefetch (_prefetchedUrl/_prefetchedVideoId)
    // is kept: playTrackInternal only uses it when the videoId matches, so a
    // stale entry is harmless and it lets a plain "next" stay instant.
    _queueGeneration++;
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
        // Only retry with the standalone resolve on a transient failure; a NOT_FOUND
        // (4xx) means the video itself is gone, so hammering it again is pointless.
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
    NexusPlayer::getInstance().play(track.videoId.c_str(), streamUrl.c_str());

    // If recommendations were piggybacked, add them to the queue
    if (isAutoplayEnabled() && !recommendations.empty()) {
        populateRecommendations(recommendations, track.title);
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
    esp_err_t err = _invidious.searchList(query, tracks, 5);
    if (err != ESP_OK || tracks.empty()) {
        ESP_LOGE(TAG, "Search failed for '%s': %s", query, esp_err_to_name(err));
        return false;
    }

    clearQueue();
    ESP_LOGI(TAG, "Search for '%s' resolved to: '%s' by '%s' [%s] (found %zu tracks)",
             query, tracks[0].title.c_str(), tracks[0].author.c_str(), tracks[0].videoId.c_str(), tracks.size());

    // Enqueue remaining search results as upcoming tracks in queue
    if (isAutoplayEnabled() && tracks.size() > 1) {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        for (size_t i = 1; i < tracks.size(); ++i) {
            _queue.push_back(tracks[i]);
        }
        ESP_LOGI(TAG, "Queued %zu related search results for upcoming autoplay", tracks.size() - 1);
    }

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
        esp_err_t err = self->_invidious.resolveWebMOpusStreamUrl(targetId, url);

        // IMPORTANT: the lock MUST be released before vTaskDelete() below.
        // vTaskDelete(NULL) never returns, so a lock_guard still in scope would
        // skip its destructor and leak _serviceMutex forever (dead task holds it),
        // which permanently hangs the next queue operation.
        {
            std::lock_guard<std::recursive_mutex> lock(self->_serviceMutex);
            if (self->_queueGeneration == gen) {
                if (err == ESP_OK && !url.empty()) {
                    self->_prefetchedVideoId = targetId;
                    self->_prefetchedUrl = url;
                    ESP_LOGI(TAG, "Asynchronously pre-fetched stream URL for upcoming track: %s", targetId.c_str());
                } else if (err == ESP_ERR_NOT_FOUND) {
                    // Video is gone - evict it from the queue now so the next advance
                    // doesn't stall on a track we already know is dead.
                    for (auto it = self->_queue.begin(); it != self->_queue.end(); ++it) {
                        if (it->videoId == targetId) { self->_queue.erase(it); break; }
                    }
                    ESP_LOGW(TAG, "Prefetch: track %s is unavailable - dropped from queue", targetId.c_str());
                } else {
                    ESP_LOGW(TAG, "Background prefetch failed for %s: %s", targetId.c_str(), esp_err_to_name(err));
                }
            } else {
                ESP_LOGD(TAG, "Prefetch for %s discarded (generation changed)", targetId.c_str());
            }
            self->_prefetchInProgress = false;
        }

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
        // Not pinned to Core 0: the HTTPS TLS handshake (ECDSA verify) is a heavy
        // CPU burst that would otherwise pile onto the WiFi + Opus-decode core and
        // starve IDLE0. Let the scheduler place it on whichever core has slack.
        ThreadConfig::CORE_ANY,
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
            ThreadConfig::CORE_ANY
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
    std::string baseAuthor;
    std::string baseTitle;
    uint32_t generation = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        if (!isAutoplayEnabled() || _replenishInProgress) return;
        if (_queue.size() > QUEUE_LOW_WATERMARK) return;

        if (!_queue.empty()) {
            baseTrackId = _queue.back().videoId;
            baseAuthor = _queue.back().author;
            baseTitle = _queue.back().title;
        } else {
            baseTrackId = _currentTrack.videoId;
            baseAuthor = _currentTrack.author;
            baseTitle = _currentTrack.title;
        }
        if (baseTrackId.empty()) return;

        _replenishInProgress = true;
        generation = _queueGeneration;
    }

    struct ReplenishContext {
        MusicPlaybackService* self;
        std::string baseId;
        std::string baseAuthor;
        std::string baseTitle;
        uint32_t generation;
        bool withCaps;
    };
    auto* ctx = new ReplenishContext{this, baseTrackId, baseAuthor, baseTitle, generation, true};

    auto taskFn = [](void* arg) {
        auto* c = static_cast<ReplenishContext*>(arg);
        MusicPlaybackService* self = c->self;
        std::string baseId = c->baseId;
        std::string baseAuthor = c->baseAuthor;
        std::string baseTitle = c->baseTitle;
        uint32_t gen = c->generation;
        bool caps = c->withCaps;
        delete c;

        // Yield CPU to let concurrent audio startup settle
        vTaskDelay(pdMS_TO_TICKS(200));

        // Bail out of the (potentially slow, multi-request) fallback chain the moment
        // a user action changes the queue - each call below serializes on the shared
        // Invidious HTTP mutex and would otherwise stall a pending track change.
        auto stale = [&]() {
            std::lock_guard<std::recursive_mutex> lock(self->_serviceMutex);
            return self->_queueGeneration != gen;
        };

        std::vector<InvidiousTrack> recs;
        esp_err_t err = self->_invidious.getRecommendedTracks(baseId, recs, 8);
        if ((err != ESP_OK || recs.empty()) && !baseAuthor.empty() && !stale()) {
            ESP_LOGI(TAG, "Recommended videos not returned for %s (%s). Falling back to search for artist '%s'...",
                     baseId.c_str(), esp_err_to_name(err), baseAuthor.c_str());
            err = self->_invidious.searchList(baseAuthor, recs, 8);
        }
        if ((err != ESP_OK || recs.empty()) && !baseTitle.empty() && !stale()) {
            ESP_LOGI(TAG, "Falling back to search for title '%s'...", baseTitle.c_str());
            err = self->_invidious.searchList(baseTitle, recs, 8);
        }

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
        ThreadConfig::CORE_ANY,  // keep heavy TLS crypto off the WiFi + decode core
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
            ThreadConfig::CORE_ANY
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

    // A user-initiated skip supersedes any background prefetch/replenish already
    // in flight - mark their results stale so they don't clobber the new track.
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
        if (nextTrack.videoId.empty()) break;  // queue drained; fall through to autoplay refill

        esp_err_t rc = playTrackInternal(nextTrack);
        if (rc == ESP_OK) return true;
        if (rc == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Track '%s' [%s] unavailable - skipping to next queued track",
                     nextTrack.title.c_str(), nextTrack.videoId.c_str());
            continue;  // dead video: try the next one
        }
        // Transient network/instance failure: don't burn through the queue, stop here.
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
                // Retry the skip loop now that the queue has fresh candidates.
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
    return false;
}

bool MusicPlaybackService::previous() {
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

