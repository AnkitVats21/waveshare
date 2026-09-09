#include "InvidiousClient.h"
#include "InvidiousInstanceResolver.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <string>

static const char* TAG = "InvidiousClient";

namespace {

std::string urlEncode(const std::string& input) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 2);

    for (unsigned char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

bool containsIgnoreCase(const std::string& haystack, const std::string& needle) {
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char ch1, char ch2) { return std::tolower(ch1) == std::tolower(ch2); }
    );
    return (it != haystack.end());
}

struct HttpLock {
    SemaphoreHandle_t sem;
    explicit HttpLock(SemaphoreHandle_t s) : sem(s) {
        if (sem) xSemaphoreTake(sem, portMAX_DELAY);
    }
    ~HttpLock() {
        if (sem) xSemaphoreGive(sem);
    }
};

} // namespace

InvidiousClient::InvidiousClient() : _forcedHost("") {
    _mutex = xSemaphoreCreateMutex();
}

InvidiousClient::InvidiousClient(const std::string& forcedHost) : _forcedHost(forcedHost) {
    _mutex = xSemaphoreCreateMutex();
}

InvidiousClient::~InvidiousClient() {
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

std::string InvidiousClient::getHost() const {
    if (!_forcedHost.empty()) return _forcedHost;
    return InvidiousInstanceResolver::getInstance().getActiveInstance();
}

esp_err_t InvidiousClient::httpEventHandler(esp_http_client_event_t* evt) {
    if (!evt) return ESP_OK;
    auto* response = static_cast<std::string*>(evt->user_data);

    if (evt->event_id == HTTP_EVENT_ON_DATA && response && evt->data && evt->data_len > 0) {
        response->append(static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

esp_err_t InvidiousClient::httpGet(const std::string& pathWithQuery, std::string& outResponse) {
    HttpLock lock(_mutex);

    const std::string host = getHost();
    if (host.empty() || pathWithQuery.empty()) return ESP_ERR_INVALID_ARG;

    std::string url;
    if (host.rfind("http://", 0) == 0 || host.rfind("https://", 0) == 0) {
        url = host + pathWithQuery;
    } else if (host.find(":") != std::string::npos) {
        url = "http://" + host + pathWithQuery;
    } else {
        url = "https://" + host + pathWithQuery;
    }

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = httpEventHandler;
    config.user_data = &outResponse;
    config.timeout_ms = 25000;
    config.buffer_size = 4096;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.skip_cert_common_name_check = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0 (ESP32-S3 Waveshare)");

    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status == 200) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Invidious request to %s failed (err=%s, status=%d)", host.c_str(), esp_err_to_name(err), status);
    if (_forcedHost.empty()) {
        InvidiousInstanceResolver::getInstance().markInstanceFailed();
    }
    return ESP_FAIL;
}

esp_err_t InvidiousClient::search(const std::string& query, InvidiousTrack& outTrack) {
    std::vector<InvidiousTrack> tracks;
    esp_err_t err = searchList(query, tracks, 1);
    if (err == ESP_OK && !tracks.empty()) {
        outTrack = tracks[0];
        return ESP_OK;
    }
    return (err != ESP_OK) ? err : ESP_ERR_NOT_FOUND;
}

esp_err_t InvidiousClient::searchList(const std::string& query, std::vector<InvidiousTrack>& outTracks, size_t limit) {
    outTracks.clear();
    if (query.empty() || limit == 0) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "searchList: searching '%s' (limit=%zu)...", query.c_str(), limit);

    const std::string path = "/api/v1/search?q=" + urlEncode(query) +
                             "&type=video&fields=videoId,title,author,lengthSeconds";

    std::string response;
    esp_err_t err = httpGet(path, response);
    if (err != ESP_OK || response.empty()) {
        ESP_LOGW(TAG, "searchList: httpGet failed for '%s' (err=%s, resp_len=%zu)",
                 query.c_str(), esp_err_to_name(err), response.length());
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    JsonDocument filter;
    filter[0]["videoId"] = true;
    filter[0]["title"] = true;
    filter[0]["author"] = true;
    filter[0]["lengthSeconds"] = true;

    JsonDocument doc;
    DeserializationError jsonErr = deserializeJson(doc, response, DeserializationOption::Filter(filter));
    if (jsonErr) {
        ESP_LOGE(TAG, "Search JSON deserialize failed: %s", jsonErr.c_str());
        return ESP_FAIL;
    }

    JsonArray arr = doc.as<JsonArray>();
    if (arr.isNull() || arr.size() == 0) {
        ESP_LOGW(TAG, "No search results returned for '%s'", query.c_str());
        return ESP_ERR_NOT_FOUND;
    }

    for (JsonObject item : arr) {
        const char* videoId = item["videoId"];
        const char* title = item["title"];
        const char* author = item["author"];
        int duration = item["lengthSeconds"] | 0;

        if (videoId && videoId[0] != '\0' && title && title[0] != '\0') {
            InvidiousTrack track;
            track.videoId = videoId;
            track.title = title;
            track.author = author ? author : "";
            track.durationSeconds = duration;
            outTracks.push_back(track);
            if (outTracks.size() >= limit) {
                break;
            }
        }
    }

    if (outTracks.empty()) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Search for '%s' returned %zu tracks (top: '%s' by '%s' [%s])",
             query.c_str(), outTracks.size(),
             outTracks[0].title.c_str(), outTracks[0].author.c_str(), outTracks[0].videoId.c_str());
    return ESP_OK;
}

esp_err_t InvidiousClient::resolveWithRecommendations(
    const std::string& videoId, 
    std::string& outUrl, 
    std::vector<InvidiousTrack>& outRecommendations,
    size_t recLimit
) {
    outRecommendations.clear();
    if (videoId.empty()) return ESP_ERR_INVALID_ARG;

    const std::string path = "/api/v1/videos/" + urlEncode(videoId) +
                             "?fields=adaptiveFormats(type,url,bitrate,qualityLabel),recommendedVideos(videoId,title,author,lengthSeconds)";

    std::string response;
    esp_err_t err = httpGet(path, response);
    if (err != ESP_OK || response.empty()) {
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    JsonDocument filter;
    filter["adaptiveFormats"][0]["type"] = true;
    filter["adaptiveFormats"][0]["url"] = true;
    filter["adaptiveFormats"][0]["bitrate"] = true;
    filter["adaptiveFormats"][0]["qualityLabel"] = true;
    filter["recommendedVideos"][0]["videoId"] = true;
    filter["recommendedVideos"][0]["title"] = true;
    filter["recommendedVideos"][0]["author"] = true;
    filter["recommendedVideos"][0]["lengthSeconds"] = true;

    JsonDocument doc;
    DeserializationError jsonErr = deserializeJson(doc, response, DeserializationOption::Filter(filter));
    if (jsonErr) {
        ESP_LOGE(TAG, "Video format/recs JSON deserialize failed: %s", jsonErr.c_str());
        return ESP_FAIL;
    }

    JsonArray formats = doc["adaptiveFormats"].as<JsonArray>();
    if (formats.isNull() || formats.size() == 0) {
        ESP_LOGE(TAG, "No adaptiveFormats found for videoId %s", videoId.c_str());
        return ESP_ERR_NOT_FOUND;
    }

    int bestBitrate = -1;
    std::string bestUrl;

    for (JsonObject f : formats) {
        const char* type = f["type"];
        const char* streamUrl = f["url"];
        int bitrate = f["bitrate"] | 0;
        const char* quality = f["qualityLabel"];

        if (!type || !streamUrl || streamUrl[0] == '\0') continue;
        if (quality && quality[0] != '\0') continue; // Skip video streams

        std::string typeStr = type;
        if (containsIgnoreCase(typeStr, "opus")) {
            if (bitrate > bestBitrate) {
                bestBitrate = bitrate;
                bestUrl = streamUrl;
            }
        }
    }

    if (bestUrl.empty()) {
        // Fallback: any audio stream
        for (JsonObject f : formats) {
            const char* type = f["type"];
            const char* streamUrl = f["url"];
            if (type && streamUrl && containsIgnoreCase(type, "audio")) {
                bestUrl = streamUrl;
                break;
            }
        }
    }

    if (bestUrl.empty()) {
        ESP_LOGE(TAG, "Failed to resolve usable audio format for %s", videoId.c_str());
        return ESP_ERR_NOT_FOUND;
    }

    outUrl = bestUrl;
    ESP_LOGI(TAG, "Resolved stream URL (bitrate=%d, length=%zu)", bestBitrate, outUrl.length());

    // Extract recommendations if available
    JsonArray recs = doc["recommendedVideos"].as<JsonArray>();
    if (!recs.isNull() && recLimit > 0) {
        for (JsonObject item : recs) {
            const char* vid = item["videoId"];
            const char* title = item["title"];
            const char* author = item["author"];
            int duration = item["lengthSeconds"] | 0;

            if (vid && vid[0] != '\0' && title && title[0] != '\0' && vid != videoId) {
                InvidiousTrack track;
                track.videoId = vid;
                track.title = title;
                track.author = author ? author : "";
                track.durationSeconds = duration;
                outRecommendations.push_back(track);
                if (outRecommendations.size() >= recLimit) break;
            }
        }
        ESP_LOGI(TAG, "Extracted %zu recommendations for %s", outRecommendations.size(), videoId.c_str());
    }

    return ESP_OK;
}

esp_err_t InvidiousClient::resolveOpusUrl(const std::string& videoId, std::string& outUrl) {
    std::vector<InvidiousTrack> dummy;
    return resolveWithRecommendations(videoId, outUrl, dummy, 0);
}

esp_err_t InvidiousClient::getRecommendedTracks(
    const std::string& currentVideoId, 
    std::vector<InvidiousTrack>& outTracks, 
    size_t limit
) {
    outTracks.clear();
    if (currentVideoId.empty()) return ESP_ERR_INVALID_ARG;

    const std::string path = "/api/v1/videos/" + urlEncode(currentVideoId) +
                             "?fields=recommendedVideos(videoId,title,author,lengthSeconds)";

    std::string response;
    esp_err_t err = httpGet(path, response);
    if (err != ESP_OK || response.empty()) {
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    JsonDocument filter;
    filter["recommendedVideos"][0]["videoId"] = true;
    filter["recommendedVideos"][0]["title"] = true;
    filter["recommendedVideos"][0]["author"] = true;
    filter["recommendedVideos"][0]["lengthSeconds"] = true;

    JsonDocument doc;
    DeserializationError jsonErr = deserializeJson(doc, response, DeserializationOption::Filter(filter));
    if (jsonErr) {
        return ESP_FAIL;
    }

    JsonArray recs = doc["recommendedVideos"].as<JsonArray>();
    if (recs.isNull() || recs.size() == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    for (JsonObject item : recs) {
        const char* vid = item["videoId"];
        const char* title = item["title"];
        const char* author = item["author"];
        int duration = item["lengthSeconds"] | 0;

        if (vid && vid[0] != '\0' && title && title[0] != '\0' && vid != currentVideoId) {
            InvidiousTrack track;
            track.videoId = vid;
            track.title = title;
            track.author = author ? author : "";
            track.durationSeconds = duration;
            outTracks.push_back(track);
            if (outTracks.size() >= limit) break;
        }
    }

    return outTracks.empty() ? ESP_ERR_NOT_FOUND : ESP_OK;
}
