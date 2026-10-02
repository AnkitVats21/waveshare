#pragma once
#include "InvidiousClient.h"
#include <cstdint>
#include <string>
#include <vector>

// Another player that can take over the music: a satellite (nexus-orbit on a
// PC or Pi) that resolves and plays the track itself. MusicPlaybackService
// keeps the queue and sends the active satellite what to play; the satellite
// reports position and state back into SysDb (media.*) and says when a track
// ends (MusicPlaybackService::onTrackFinished).
//
// Implemented in main (OrbitChannel); media_player only sees this interface.
// Calls must not block: implementations queue the message and return.
class RemoteOutput {
public:
    struct Satellite {
        std::string id;
        std::string name;
        bool active;
    };

    virtual ~RemoteOutput() = default;

    // Commands to the active satellite.
    virtual void play(const InvidiousTrack& track, uint32_t position_ms) = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void seek(uint32_t position_ms) = 0;
    virtual void stop() = 0;

    // The connected satellites.
    virtual std::vector<Satellite> satellites() = 0;

    // Moves playback to the satellite whose id or name matches `target`
    // (case-insensitive; a name may be given in part), or to the board for
    // "local". Sets `chosen` to the satellite's name, or "local". False when
    // no satellite matches.
    virtual bool select(const std::string& target, std::string& chosen) = 0;
};
