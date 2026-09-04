#pragma once

#include <string>
#include "esp_err.h"

struct InvidiousTrack {
    std::string videoId;
    std::string title;
    std::string author;
    int durationSeconds = 0;
};

#include "esp_http_client.h"

class InvidiousClient {
public:
    InvidiousClient();
    explicit InvidiousClient(const std::string& forcedHost);

    esp_err_t search(const std::string& query, InvidiousTrack& outTrack);
    esp_err_t resolveOpusUrl(const std::string& videoId, std::string& outUrl);
    esp_err_t getRecommendedTrack(const std::string& currentVideoId, InvidiousTrack& outTrack);

    std::string getCurrentHost() const;

private:
    std::string _forcedHost;

    std::string getHost() const;
    esp_err_t httpGet(const std::string& pathWithQuery, std::string& outResponse);
    static esp_err_t httpEventHandler(esp_http_client_event_t* evt);
};
