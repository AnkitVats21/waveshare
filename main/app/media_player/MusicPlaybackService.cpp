#include "MusicPlaybackService.h"

#include "esp_log.h"

static const char* TAG = "MusicPlayback";

// Initial instance. Keep this in one place so it can later be moved to Kconfig/NVS
// without changing the rest of the playback architecture.
static constexpr const char* INVIDIOUS_INSTANCE = "inv.tux.pizza";

MusicPlaybackService& MusicPlaybackService::getInstance() {
    static MusicPlaybackService instance;
    return instance;
}

MusicPlaybackService::MusicPlaybackService()
    : _invidious(INVIDIOUS_INSTANCE) {}

bool MusicPlaybackService::begin() {
    if (_initialized) return true;
    _initialized = true;
    ESP_LOGI(TAG, "Music playback service initialized (Invidious: %s)", INVIDIOUS_INSTANCE);
    return true;
}

bool MusicPlaybackService::resolveAndPlay(const char* query, bool clearCurrent) {
    if (!query || query[0] == '\0') {
        ESP_LOGE(TAG, "Empty music query");
        return false;
    }

    InvidiousTrack track;
    esp_err_t err = _invidious.search(query, track);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Invidious search failed for '%s': %s", query, esp_err_to_name(err));
        return false;
    }

    std::string streamUrl;
    err = _invidious.resolveOpusUrl(track.videoId, streamUrl);
    if (err != ESP_OK || streamUrl.empty()) {
        ESP_LOGE(TAG, "Failed to resolve Opus stream for %s: %s",
                 track.videoId.c_str(), esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Playing '%s' by '%s'", track.title.c_str(), track.author.c_str());

    // NexusPlayer already owns the download/cache/decode pipeline. Invidious
    // only supplies the identity and resolved stream URL.
    NexusPlayer::getInstance().play(track.videoId.c_str(), streamUrl.c_str());

    _currentVideoId = track.videoId;
    _currentTitle = track.title;
    (void)clearCurrent; // Queue management is intentionally the next phase.
    return true;
}

bool MusicPlaybackService::play(const char* query) {
    return resolveAndPlay(query, true);
}

bool MusicPlaybackService::playNext(const char* query) {
    // The initial migration treats play_next as immediate resolution only after
    // the current queue model is introduced. Keep the command local now rather
    // than routing it through MQTT.
    return resolveAndPlay(query, false);
}

bool MusicPlaybackService::next() {
    ESP_LOGW(TAG, "next() has no local queue yet");
    return false;
}

bool MusicPlaybackService::previous() {
    ESP_LOGW(TAG, "previous() has no local history yet");
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
    _currentVideoId.clear();
    _currentTitle.clear();
}
