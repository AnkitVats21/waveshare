#pragma once

#include "InvidiousClient.h"
#include "app/media_player/NexusPlayer.h"
#include <string>

class MusicPlaybackService {
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

private:
    MusicPlaybackService();

    InvidiousClient _invidious;
    bool _initialized = false;
    std::string _currentVideoId;
    std::string _currentTitle;

    bool resolveAndPlay(const char* query, bool clearCurrent);
};
