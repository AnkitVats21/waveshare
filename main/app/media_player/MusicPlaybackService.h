#pragma once

#include "InvidiousClient.h"
#include "app/media_player/NexusPlayer.h"
#include "app/media_player/IPlaybackObserver.h"
#include <string>
#include <deque>
#include <vector>

enum class RepeatMode {
    Off,
    One,
    All
};

class MusicPlaybackService : public IPlaybackObserver {
public:
    static MusicPlaybackService& getInstance();

    bool begin();
    
    bool play(const char* query);
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
    void setRepeatMode(RepeatMode mode) { _repeatMode = mode; }
    RepeatMode getRepeatMode() const { return _repeatMode; }
    void setAutoplay(bool enabled);
    bool isAutoplayEnabled() const;
    void setCaching(bool enabled);
    bool isCachingEnabled() const;

    const InvidiousTrack& getCurrentTrack() const { return _currentTrack; }
    const std::deque<InvidiousTrack>& getQueue() const { return _queue; }
    const std::vector<InvidiousTrack>& getHistory() const { return _history; }

    InvidiousClient& getInvidiousClient() { return _invidious; }
    void populateRecommendations(const std::vector<InvidiousTrack>& recs, const std::string& title);

    // IPlaybackObserver implementation
    void onTrackStarted(const char* songId) override;
    void onTrackFinished(const char* songId) override;
    void onPlaybackError(const char* songId, int errorCode) override;

private:
    MusicPlaybackService();
    ~MusicPlaybackService() override = default;

    InvidiousClient _invidious;
    bool _initialized = false;
    bool _autoplayEnabled = true;
    RepeatMode _repeatMode = RepeatMode::Off;

    InvidiousTrack _currentTrack;
    std::deque<InvidiousTrack> _queue;
    std::vector<InvidiousTrack> _history;

    std::string _prefetchedVideoId;
    std::string _prefetchedUrl;

    bool playTrack(const InvidiousTrack& track);
    bool playTrackFallback(const InvidiousTrack& track);
    bool resolveAndPlayImmediate(const char* query);
    void prefetchNextTrack();
};
