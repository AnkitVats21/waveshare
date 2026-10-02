#pragma once
#include <cstdint>

// The hub, when it runs the music (hub mode; docs/plans/hub-relay-design.md
// section 13, nexus-orbit docs/PROTOCOL.md section 5). The board is then a
// player and a controller: what the person does here (keys, dashboard, voice
// in direct mode) goes to the hub as an action, and the board plays what the
// hub sends (MusicPlaybackService::hubPlay ...), with a copy of its queue to
// carry on alone if the link drops.
//
// Implemented in main over HubLink (HubMusic). Calls must not block.
class MusicHub {
public:
    // A music_action. Unset fields are left out.
    struct Action {
        const char* action;              // play, play_next, queue, next, previous, pause, resume,
                                         // toggle, stop, replay, seek, remove, clear, shuffle,
                                         // repeat, autoplay, output
        const char* query = nullptr;
        const char* id = nullptr;
        bool cached = false;             // with id: the song is saved on this board's card
        int64_t position_ms = -1;
        int index = -1;
        const char* mode = nullptr;      // repeat: off, one, all
        int on = -1;                     // autoplay: 0 or 1
        const char* target = nullptr;    // output
    };

    virtual ~MusicHub() = default;

    // The link is up and the hub runs the music.
    virtual bool active() const = 0;
    // False if it could not be sent (the caller then acts locally).
    virtual bool sendAction(const Action& action) = 0;
    // The song the hub sent (seq) ended, or failed: the hub picks what's next.
    virtual void playerEvent(uint32_t seq, const char* id, const char* event, const char* reason = nullptr) = 0;
};
