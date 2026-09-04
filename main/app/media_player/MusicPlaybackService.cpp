#include "MusicPlaybackService.h"
#include "esp_log.h"

static const char* TAG = "MusicPlayback";

MusicPlaybackService& MusicPlaybackService::getInstance() {
    static MusicPlaybackService instance;
    return instance;
}

MusicPlaybackService::MusicPlaybackService() {}

bool MusicPlaybackService::begin() {
    if (_initialized) return true;
    
    NexusPlayer::getInstance().addObserver(this);
    _initialized = true;
    ESP_LOGI(TAG, "MusicPlaybackService initialized and registered as NexusPlayer observer");
    return true;
}

#include <algorithm>
#include <random>

bool MusicPlaybackService::playTrack(const InvidiousTrack& track) {
    if (track.videoId.empty()) return false;

    // Check if the track is already cached locally on the SD card
    if (NexusPlayer::getInstance().getStorageManager().fileExists(track.videoId.c_str())) {
        ESP_LOGI(TAG, "Track '%s' [%s] found in local cache! Playing immediately (0ms network delay)",
                 track.title.c_str(), track.videoId.c_str());
        _currentTrack = track;
        NexusPlayer::getInstance().play(track.videoId.c_str(), "");
        return true;
    }

    std::string streamUrl;
    if (_prefetchedVideoId == track.videoId && !_prefetchedUrl.empty()) {
        ESP_LOGI(TAG, "Using pre-fetched stream URL for '%s' (0ms network delay!)", track.videoId.c_str());
        streamUrl = _prefetchedUrl;
        _prefetchedUrl.clear();
        _prefetchedVideoId.clear();
    } else {
        esp_err_t err = _invidious.resolveOpusUrl(track.videoId, streamUrl);
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
    return true;
}

bool MusicPlaybackService::resolveAndPlayImmediate(const char* query) {
    if (!query || query[0] == '\0') {
        ESP_LOGE(TAG, "Empty music query");
        return false;
    }

    InvidiousTrack track;
    esp_err_t err = _invidious.search(query, track);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Search failed for '%s': %s", query, esp_err_to_name(err));
        return false;
    }

    return playTrack(track);
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
    if (_queue.empty()) return;
    const std::string& nextId = _queue.front().videoId;
    if (NexusPlayer::getInstance().getStorageManager().fileExists(nextId.c_str())) {
        return; // Already cached on SD card
    }
    if (_prefetchedVideoId == nextId && !_prefetchedUrl.empty()) {
        return; // Already prefetched
    }

    std::string url;
    if (_invidious.resolveOpusUrl(nextId, url) == ESP_OK && !url.empty()) {
        _prefetchedVideoId = nextId;
        _prefetchedUrl = url;
        ESP_LOGI(TAG, "Pre-fetched stream URL for upcoming track: %s", nextId.c_str());
    }
}

bool MusicPlaybackService::next() {
    ESP_LOGI(TAG, "Advancing to next track");
    if (!_queue.empty()) {
        InvidiousTrack nextTrack = _queue.front();
        _queue.pop_front();
        return playTrack(nextTrack);
    }

    if (_autoplayEnabled && !_currentTrack.videoId.empty()) {
        ESP_LOGI(TAG, "Queue empty; attempting autoplay recommendation for %s", _currentTrack.videoId.c_str());
        InvidiousTrack recTrack;
        esp_err_t err = _invidious.getRecommendedTrack(_currentTrack.videoId, recTrack);
        if (err == ESP_OK && !recTrack.videoId.empty()) {
            return playTrack(recTrack);
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
    if (!_queue.empty()) {
        next();
    }
}
