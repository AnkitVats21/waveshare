#pragma once

#include <string>
#include "esp_err.h"

struct InvidiousTrack {
    std::string videoId;
    std::string title;
    std::string author;
    int durationSeconds = 0;
};

class InvidiousClient {
public:
    explicit InvidiousClient(const char* instanceHost);

    esp_err_t search(const std::string& query, InvidiousTrack& outTrack);
    esp_err_t resolveOpusUrl(const std::string& videoId, std::string& outUrl);

private:
    std::string _instanceHost;

    esp_err_t httpGet(const std::string& url, std::string& outResponse);
    static esp_err_t httpEventHandler(void* event);
};
