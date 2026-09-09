#include "MusicPlaybackService.h"
#include "esp_log.h"
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

void MusicPlaybackService::populateRecommendations(const std::vector<InvidiousTrack>& recs, const std::string& title) {
    if (recs.empty()) return;
    for (const auto& r : recs) {
        _queue.push_back(r);
    }
    ESP_LOGI(TAG, "Populated queue with %zu recommended tracks for '%s'", _queue.size(), title.c_str());
    prefetchNextTrack();
}

bool MusicPlaybackService::playTrack(const InvidiousTrack& track) {
    if (track.videoId.empty()) return false;

    // Check if the track is already cached locally on the SD card
    if (NexusPlayer::getInstance().getStorageManager().fileExists(track.videoId.c_str())) {
        ESP_LOGI(TAG, "Track '%s' [%s] found in local cache! Playing immediately (0ms network delay)",
                 track.title.c_str(), track.videoId.c_str());
        _currentTrack = track;
        NexusPlayer::getInstance().play(track.videoId.c_str(), "");

        // If queue is empty, asynchronously populate recommendations in background so playback starts instantly
        if (_queue.empty() && isAutoplayEnabled()) {
            std::string vid = track.videoId;
            std::string tTitle = track.title;
            xTaskCreate([](void* arg) {
                auto* info = static_cast<std::pair<std::string, std::string>*>(arg);
                std::vector<InvidiousTrack> recs;
                if (MusicPlaybackService::getInstance().getInvidiousClient().getRecommendedTracks(info->first, recs, 8) == ESP_OK) {
                    MusicPlaybackService::getInstance().populateRecommendations(recs, info->second);
                }
                delete info;
                vTaskDelete(NULL);
            }, "bg_recs", ThreadConfig::StackSize::STACK_PLAYER, new std::pair<std::string, std::string>(vid, tTitle), ThreadConfig::Priority::LOW, NULL);
        }
        return true;
    }

    std::string streamUrl;
    std::vector<InvidiousTrack> recommendations;

    if (_prefetchedVideoId == track.videoId && !_prefetchedUrl.empty()) {
        ESP_LOGI(TAG, "Using pre-fetched stream URL for '%s' (0ms network delay!)", track.videoId.c_str());
        streamUrl = _prefetchedUrl;
        _prefetchedUrl.clear();
        _prefetchedVideoId.clear();
    } else {
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

    _currentTrack = track;
    NexusPlayer::getInstance().play(track.videoId.c_str(), streamUrl.c_str());

    // If queue is empty, populate it with recommendations from this track
    if (_queue.empty() && isAutoplayEnabled()) {
        if (!recommendations.empty()) {
            populateRecommendations(recommendations, track.title);
        }
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

    _currentTrack = track;
    NexusPlayer::getInstance().play(track.videoId.c_str(), streamUrl.c_str());
    return true;
}

bool MusicPlaybackService::resolveAndPlayImmediate(const char* query) {
    if (!query || query[0] == '\0') {
        ESP_LOGE(TAG, "Empty music query");
        return false;
    }

    // Stop current playback immediately to clear network, decoding, and Core 1 AEC load
    NexusPlayer::getInstance().stop();

    std::vector<InvidiousTrack> tracks;
    esp_err_t err = _invidious.searchList(query, tracks, 1);
    if (err != ESP_OK || tracks.empty()) {
        ESP_LOGE(TAG, "Search failed for '%s': %s", query, esp_err_to_name(err));
        return false;
    }

    _queue.clear();
    ESP_LOGI(TAG, "Search for '%s' resolved to: '%s' by '%s' [%s]",
             query, tracks[0].title.c_str(), tracks[0].author.c_str(), tracks[0].videoId.c_str());

    return playTrack(tracks[0]);
}

bool MusicPlaybackService::play(const char* query) {
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
        _queue.push_front(track);
        ESP_LOGI(TAG, "Queued to play next: '%s' (queue depth: %zu)", track.title.c_str(), _queue.size());
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
        _queue.push_back(track);
        ESP_LOGI(TAG, "Queued track: '%s' (queue depth: %zu)", track.title.c_str(), _queue.size());
        if (_queue.size() == 1) {
            prefetchNextTrack();
        }
        return true;
    }
}

void MusicPlaybackService::clearQueue() {
    _queue.clear();
    _prefetchedUrl.clear();
    _prefetchedVideoId.clear();
    ESP_LOGI(TAG, "Playback queue cleared");
}

void MusicPlaybackService::shuffleQueue() {
    if (_queue.size() <= 1) return;
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(_queue.begin(), _queue.end(), g);
    ESP_LOGI(TAG, "Playback queue shuffled (%zu tracks)", _queue.size());
    prefetchNextTrack();
}

void MusicPlaybackService::prefetchNextTrack() {
    if (_queue.empty() || _prefetchInProgress) return;
    const std::string nextId = _queue.front().videoId;
    if (nextId.empty()) return;

    if (NexusPlayer::getInstance().getStorageManager().fileExists(nextId.c_str())) {
        return; // Already cached on SD card
    }
    if (_prefetchedVideoId == nextId && !_prefetchedUrl.empty()) {
        return; // Already prefetched
    }

    _prefetchInProgress = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        [](void* arg) {
            auto* self = static_cast<MusicPlaybackService*>(arg);
            // Yield CPU so playback startup, I2S DMA, and AFE processing settle cleanly
            vTaskDelay(pdMS_TO_TICKS(150));

            std::string targetId;
            if (!self->_queue.empty()) {
                targetId = self->_queue.front().videoId;
            }

            if (!targetId.empty()) {
                std::string url;
                if (self->_invidious.resolveOpusUrl(targetId, url) == ESP_OK && !url.empty()) {
                    if (!self->_queue.empty() && self->_queue.front().videoId == targetId) {
                        self->_prefetchedVideoId = targetId;
                        self->_prefetchedUrl = url;
                        ESP_LOGI(TAG, "Asynchronously pre-fetched stream URL for upcoming track: %s", targetId.c_str());
                    }
                }
            }
            self->_prefetchInProgress = false;
            vTaskDelete(NULL);
        },
        "bg_prefetch",
        ThreadConfig::StackSize::STACK_PLAYER,
        this,
        ThreadConfig::Priority::LOW,
        NULL,
        ThreadConfig::CORE_NETWORK
    );

    if (ret != pdPASS) {
        _prefetchInProgress = false;
        ESP_LOGW(TAG, "Failed to spawn background prefetch task");
    }
}

bool MusicPlaybackService::next() {
    ESP_LOGI(TAG, "Advancing to next track");
    if (!_queue.empty()) {
        InvidiousTrack nextTrack = _queue.front();
        _queue.pop_front();
        return playTrack(nextTrack);
    }

    if (isAutoplayEnabled() && !_currentTrack.videoId.empty()) {
        ESP_LOGI(TAG, "Queue empty; attempting autoplay recommendations for %s", _currentTrack.videoId.c_str());
        std::vector<InvidiousTrack> recTracks;
        esp_err_t err = _invidious.getRecommendedTracks(_currentTrack.videoId, recTracks, 8);
        if (err == ESP_OK && !recTracks.empty()) {
            InvidiousTrack first = recTracks[0];
            for (size_t i = 1; i < recTracks.size(); ++i) {
                _queue.push_back(recTracks[i]);
            }
            ESP_LOGI(TAG, "Autoplay populated %zu recommended tracks (playing: '%s')", recTracks.size(), first.title.c_str());
            return playTrack(first);
        }
    }

    ESP_LOGI(TAG, "No more tracks in queue or recommendations");
    NexusPlayer::getInstance().stop();
    return false;
}

bool MusicPlaybackService::previous() {
    ESP_LOGI(TAG, "Going back to previous track");
    if (_history.empty()) {
        ESP_LOGW(TAG, "No track history available");
        return false;
    }

    InvidiousTrack prevTrack = _history.back();
    _history.pop_back();

    if (!_currentTrack.videoId.empty()) {
        _queue.push_front(_currentTrack);
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
    _currentTrack = InvidiousTrack();
}

void MusicPlaybackService::onTrackStarted(const char* songId) {
    ESP_LOGI(TAG, "Observer event: Track started [%s]", songId ? songId : "");
    prefetchNextTrack();
}

void MusicPlaybackService::onTrackFinished(const char* songId) {
    ESP_LOGI(TAG, "Observer event: Track finished [%s]", songId ? songId : "");
    if (_repeatMode == RepeatMode::One && !_currentTrack.videoId.empty()) {
        ESP_LOGI(TAG, "Repeating single track: %s", _currentTrack.title.c_str());
        playTrack(_currentTrack);
        return;
    }

    if (!_currentTrack.videoId.empty()) {
        if (_repeatMode == RepeatMode::All) {
            _queue.push_back(_currentTrack);
        }
        _history.push_back(_currentTrack);
        if (_history.size() > 20) {
            _history.erase(_history.begin());
        }
    }
    next();
}

void MusicPlaybackService::onPlaybackError(const char* songId, int errorCode) {
    ESP_LOGE(TAG, "Observer event: Playback error %d for [%s]", errorCode, songId ? songId : "");
    if (errorCode == -1 && songId && _currentTrack.videoId == songId) {
        ESP_LOGW(TAG, "Local file error for %s. Deleting corrupted cache and falling back to live stream!", songId);
        NexusPlayer::getInstance().getStorageManager().deleteFile(songId);
        if (playTrackFallback(_currentTrack)) {
            return;
        }
    }
    if (!_queue.empty()) {
        next();
    }
}
