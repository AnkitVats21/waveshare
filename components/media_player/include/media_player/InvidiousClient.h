#pragma once

#include "TrackSource.h"
#include <string>
#include <vector>
#include "esp_err.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Looks songs up on an Invidious instance directly from the board: the
// fallback TrackSource when the MCP server can't be reached
// (CONFIG_WAVESHARE_INVIDIOUS_FALLBACK).
class InvidiousClient : public TrackSource {
public:
    InvidiousClient();
    explicit InvidiousClient(const std::string& forcedHost);
    ~InvidiousClient();

    esp_err_t search(const std::string& query, InvidiousTrack& outTrack) override;
    esp_err_t searchList(const std::string& query, std::vector<InvidiousTrack>& outTracks, size_t limit = 10) override;
    // The direct WebM container audio stream (Opus frames).
    esp_err_t resolveStream(const std::string& videoId, std::string& outUrl) override;
    esp_err_t resolveWithRecommendations(const std::string& videoId,
                                         std::string& outUrl,
                                         std::vector<InvidiousTrack>& outRecommendations,
                                         size_t recLimit = 8) override;
    esp_err_t recommendations(const std::string& currentVideoId,
                              std::vector<InvidiousTrack>& outTracks,
                              size_t limit = 8) override;

private:
    std::string _forcedHost;
    SemaphoreHandle_t _mutex = nullptr;

    std::string getHost() const;
    esp_err_t httpGet(const std::string& pathWithQuery, std::string& outResponse);
    static esp_err_t httpEventHandler(esp_http_client_event_t* evt);
};
