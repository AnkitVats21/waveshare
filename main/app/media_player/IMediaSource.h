#pragma once

#include <string>
#include <functional>

/**
 * @brief Abstract interface for Media Resolution and Source Fetching.
 *
 * Decouples NexusPlayer and MusicPlaybackService from specific media providers.
 *
 * Supported implementations:
 *   1. Direct Mode: InvidiousClient / InvidiousInstanceResolver (YouTube via WAN)
 *   2. Relayed Mode: RelayMediaClient (Raspberry Pi Local SSD / LAN proxy)
 */
class IMediaSource {
public:
    virtual ~IMediaSource() = default;

    struct TrackMetadata {
        std::string songId;
        std::string title;
        std::string author;
        std::string streamUrl;
        int durationSeconds{0};
        bool valid{false};
    };

    using ResolveCallback = std::function<void(const TrackMetadata& track)>;

    /**
     * @brief Resolve a plain-text search query or song title to a playable stream URL.
     * @param query Search string (e.g. "Hotel California")
     * @param onResolved Callback invoked when track is resolved or failed
     * @return true if resolution was dispatched, false otherwise
     */
    virtual bool resolveTrack(const char* query, ResolveCallback onResolved) = 0;
};
