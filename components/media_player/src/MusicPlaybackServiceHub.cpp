// MusicPlaybackService in hub mode (MusicHub.h): the person's actions go to
// the hub; the board plays what the hub sends and keeps a copy of its queue.
#include "MusicPlaybackService.h"
#include "media_player/CatalogDB.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>

static const char* TAG = "MusicHubMode";

bool MusicPlaybackService::forwardToHub(MediaCmdType type, const char* query) {
    MusicHub::Action a{nullptr};
    switch (type) {
        case MediaCmdType::PLAY:
            // A stream URL (the API's test path) plays here, as before.
            if (!query || strncmp(query, "http://", 7) == 0 || strncmp(query, "https://", 8) == 0) return false;
            a.action = "play";
            a.query = query;
            break;
        case MediaCmdType::PLAY_NEXT: a.action = "play_next"; a.query = query; break;
        case MediaCmdType::QUEUE:     a.action = "queue";     a.query = query; break;
        case MediaCmdType::NEXT:      a.action = "next"; break;
        case MediaCmdType::PREVIOUS:  a.action = "previous"; break;
        case MediaCmdType::PAUSE:     a.action = "pause"; break;
        case MediaCmdType::RESUME:    a.action = "resume"; break;
        case MediaCmdType::TOGGLE_PLAY_PAUSE: a.action = "toggle"; break;
        case MediaCmdType::STOP:      a.action = "stop"; break;
        case MediaCmdType::REPLAY:    a.action = "replay"; break;
        case MediaCmdType::SEEK:
            a.action = "seek";
            a.position_ms = query ? static_cast<int64_t>(strtoul(query, nullptr, 10)) : 0;
            break;
        default:
            return false;   // output switches and the hub's own plays stay here
    }
    if ((type == MediaCmdType::PLAY || type == MediaCmdType::PLAY_NEXT || type == MediaCmdType::QUEUE) &&
        (!query || query[0] == '\0')) {
        return false;
    }
    return _hub->sendAction(a);
}

bool MusicPlaybackService::hubOutput(const char* target) {
    if (!hubActive() || !target) return false;
    MusicHub::Action a{"output"};
    a.target = target;
    return _hub->sendAction(a);
}

void MusicPlaybackService::hubPlay(const InvidiousTrack& track, const std::string& url, uint32_t positionMs,
                                   bool paused, uint32_t seq) {
    if (track.videoId.empty()) return;
    NexusPlayer::getInstance().yieldAlarm();
    {
        std::lock_guard<std::mutex> lock(_hubMutex);
        _hubTrack = track;
        _hubUrl = url;
        _hubPos = positionMs;
        _hubPaused = paused;
        _hubPendingSeq = seq;
    }
    // The URL is too long for a MediaCommand: the worker takes it from here.
    dispatch(MediaCmdType::HUB_PLAY);
}

// Media worker: the song the hub sent. From the card if it's saved there,
// else the hub's stream URL, else one looked up here.
void MusicPlaybackService::playFromHub() {
    InvidiousTrack track;
    std::string url;
    uint32_t posMs, seq;
    bool paused;
    {
        std::lock_guard<std::mutex> lock(_hubMutex);
        track = _hubTrack;
        url.swap(_hubUrl);
        posMs = _hubPos;
        paused = _hubPaused;
        seq = _hubPendingSeq;
        _hubTrack = InvidiousTrack();
    }
    if (track.videoId.empty()) return;

    invalidateBackgroundWork();
    _hubSeq = seq;
    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        _currentTrack = track;
    }
    CatalogDB::getInstance().noteTrack(track.videoId.c_str(), track.title, track.author, track.durationSeconds * 1000);
    CatalogDB::getInstance().recordPlay(track.videoId.c_str());
    postAuxCommand(MediaAuxCmdType::FETCH_THUMBNAIL, track.videoId.c_str(), track.author.c_str(), track.title.c_str());

    const bool cached = NexusPlayer::getInstance().getStorageManager().fileExists(track.videoId.c_str());
    if (!cached && url.empty()) {
        esp_err_t err = _source->resolveStream(track.videoId, url);
        if (err != ESP_OK || url.empty()) {
            ESP_LOGW(TAG, "No stream for '%s' [%s] (%s)", track.title.c_str(), track.videoId.c_str(), esp_err_to_name(err));
            if (_hub) _hub->playerEvent(seq, track.videoId.c_str(), "error", err == ESP_ERR_NOT_FOUND ? "not_found" : "lookup");
            return;
        }
    }

    ESP_LOGI(TAG, "Hub plays '%s' [%s] at %u ms%s (seq %u, %s)", track.title.c_str(), track.videoId.c_str(),
             (unsigned)posMs, paused ? ", paused" : "", (unsigned)seq, cached ? "card" : "stream");
    EmbeddedSysDb::getInstance().mutate([&track, posMs](SystemState& s) {
        s.media.state = MediaPlaybackState::PLAYING;
        s.media.output_target = MediaOutputTarget::LOCAL;
        strncpy(s.media.active_song_id, track.videoId.c_str(), sizeof(s.media.active_song_id) - 1);
        s.media.active_song_id[sizeof(s.media.active_song_id) - 1] = '\0';
        strncpy(s.media.title, track.title.c_str(), sizeof(s.media.title) - 1);
        s.media.title[sizeof(s.media.title) - 1] = '\0';
        strncpy(s.media.artist, track.author.c_str(), sizeof(s.media.artist) - 1);
        s.media.artist[sizeof(s.media.artist) - 1] = '\0';
        s.media.duration_ms = track.durationSeconds * 1000;
        s.media.position_ms = posMs;
    });
    NexusPlayer::getInstance().playAt(track.videoId.c_str(), cached ? "" : url.c_str(), posMs);
    if (paused) NexusPlayer::getInstance().pause();
}

void MusicPlaybackService::hubLost() {
    if (getCurrentTrack().videoId.empty()) return;
    ESP_LOGI(TAG, "The hub is away: this board carries on with its queue");
    if (isAutoplayEnabled()) checkAndReplenishQueue();
    prefetchNextTrack();
}

void MusicPlaybackService::hubPause() {
    NexusPlayer::getInstance().pause();
}

void MusicPlaybackService::hubResume() {
    NexusPlayer::getInstance().resume();
}

void MusicPlaybackService::hubSeek(uint32_t positionMs) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", (unsigned)positionMs);
    dispatch(MediaCmdType::SEEK, buf);
}

void MusicPlaybackService::hubStop() {
    NexusPlayer::getInstance().stop();
    invalidateBackgroundWork();
    _hubSeq = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        _currentTrack = InvidiousTrack();
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
}

void MusicPlaybackService::hubQueue(std::deque<InvidiousTrack> tracks, RepeatMode repeat, bool autoplay) {
    {
        std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
        _queue = std::move(tracks);
        _prefetchedUrl.clear();
        _prefetchedVideoId.clear();
        _repeatMode = repeat;
        _autoplayEnabled = autoplay;
    }
    EmbeddedSysDb::getInstance().mutate([repeat, autoplay](SystemState& s) {
        s.media.repeat_mode = static_cast<uint8_t>(repeat);
        s.media.autoplay_enabled = autoplay;
    });
}
