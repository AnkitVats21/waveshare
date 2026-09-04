#pragma once

#include "InvidiousClient.h"
#include "app/media_player/NexusPlayer.h"
#include "app/media_player/IPlaybackObserver.h"
#include <string>
#include <deque>
#include <vector>

class MusicPlaybackService : public IPlaybackObserver {
public:
    static MusicPlaybackService& getInstance();

    bool begin();
    
    bool play(const char* query);
    bool playNext(const char* query);
    bool next();
    bool previous();
    void pause();
    void resume();
    void stop();
    void setAutoplay(bool enabled) { _autoplayEnabled = enabled; }

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

    InvidiousTrack _currentTrack;
    std::deque<InvidiousTrack> _queue;
    std::vector<InvidiousTrack> _history;

    bool playTrack(const InvidiousTrack& track);
    bool resolveAndPlayImmediate(const char* query);
};
