#pragma once

#include <string>
#include <vector>
#include "esp_err.h"

// A song as the queue holds it (the name dates from when Invidious was the
// only source).
struct InvidiousTrack {
    std::string videoId;
    std::string title;
    std::string author;
    int durationSeconds = 0;
};

// Where songs are looked up: search, the audio stream URL of a video, and
// related songs for autoplay. Implemented in main over MCP (the nexus-mcp
// server's device-only music tools), and by InvidiousClient as the optional
// fallback (CONFIG_WAVESHARE_INVIDIOUS_FALLBACK).
//
// Results: ESP_OK; ESP_ERR_NOT_FOUND when the video is gone or nothing
// matches (callers skip it); anything else is a transient failure.
// Calls block on the network: call from tasks with stack for HTTPS + JSON.
class TrackSource {
public:
    virtual ~TrackSource() = default;

    virtual esp_err_t search(const std::string& query, InvidiousTrack& outTrack) = 0;
    virtual esp_err_t searchList(const std::string& query, std::vector<InvidiousTrack>& outTracks,
                                 size_t limit = 10) = 0;
    // The WebM/Opus audio stream of a video.
    virtual esp_err_t resolveStream(const std::string& videoId, std::string& outUrl) = 0;
    // Both in one request.
    virtual esp_err_t resolveWithRecommendations(const std::string& videoId, std::string& outUrl,
                                                 std::vector<InvidiousTrack>& outRecommendations,
                                                 size_t recLimit = 8) = 0;
    virtual esp_err_t recommendations(const std::string& videoId, std::vector<InvidiousTrack>& outTracks,
                                      size_t limit = 8) = 0;
};
