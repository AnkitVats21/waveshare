#pragma once

class IPlaybackObserver {
public:
    virtual ~IPlaybackObserver() = default;

    /**
     * @brief Called when a new track begins playback.
     */
    virtual void onTrackStarted(const char* songId) = 0;

    /**
     * @brief Called when the current track finishes playback naturally (EOF).
     */
    virtual void onTrackFinished(const char* songId) = 0;

    /**
     * @brief Called when an unrecoverable playback error occurs.
     */
    virtual void onPlaybackError(const char* songId, int errorCode) = 0;
};
