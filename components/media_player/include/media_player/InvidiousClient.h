#pragma once

#include <string>
#include <vector>
#include "esp_err.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct InvidiousTrack {
    std::string videoId;
    std::string title;
    std::string author;
    int durationSeconds = 0;
};

class InvidiousClient {
public:
    InvidiousClient();
    explicit InvidiousClient(const std::string& forcedHost);
    ~InvidiousClient();

    esp_err_t search(const std::string& query, InvidiousTrack& outTrack);
    esp_err_t searchList(const std::string& query, std::vector<InvidiousTrack>& outTracks, size_t limit = 10);
    // Resolves the direct WebM container audio stream (containing Opus codec frames) from Invidious/YouTube
    esp_err_t resolveWebMOpusStreamUrl(const std::string& videoId, std::string& outUrl);
    // Backward-compatible alias for resolveWebMOpusStreamUrl
    esp_err_t resolveOpusUrl(const std::string& videoId, std::string& outUrl) {
        return resolveWebMOpusStreamUrl(videoId, outUrl);
    }
    esp_err_t resolveWithRecommendations(const std::string& videoId, 
                                         std::string& outUrl, 
                                         std::vector<InvidiousTrack>& outRecommendations,
                                         size_t recLimit = 8);
    esp_err_t getRecommendedTracks(const std::string& currentVideoId, 
                                   std::vector<InvidiousTrack>& outTracks, 
                                   size_t limit = 8);

private:
    std::string _forcedHost;
    SemaphoreHandle_t _mutex = nullptr;

    std::string getHost() const;
    esp_err_t httpGet(const std::string& pathWithQuery, std::string& outResponse);
    static esp_err_t httpEventHandler(esp_http_client_event_t* evt);
};
