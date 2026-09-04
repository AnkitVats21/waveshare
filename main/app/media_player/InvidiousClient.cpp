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

} // namespace

InvidiousClient::InvidiousClient() : _forcedHost("") {}

InvidiousClient::InvidiousClient(const std::string& forcedHost) : _forcedHost(forcedHost) {}

std::string InvidiousClient::getHost() const {
    if (!_forcedHost.empty()) return _forcedHost;
    return InvidiousInstanceResolver::getInstance().getActiveInstance();
}

std::string InvidiousClient::getCurrentHost() const {
    return getHost();
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
    if (query.empty()) return ESP_ERR_INVALID_ARG;

    const std::string path = "/api/v1/search?q=" + urlEncode(query) +
                             "&type=video&fields=videoId,title,author,lengthSeconds";

    std::string response;
    esp_err_t err = httpGet(path, response);
    if (err != ESP_OK || response.empty()) {
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

    JsonObject first = arr[0];
    const char* videoId = first["videoId"];
    const char* title = first["title"];
    const char* author = first["author"];
    int duration = first["lengthSeconds"] | 0;

    if (!videoId || !title) {
        return ESP_ERR_NOT_FOUND;
    }

    outTrack.videoId = videoId;
    outTrack.title = title;
    outTrack.author = author ? author : "";
    outTrack.durationSeconds = duration;

    ESP_LOGI(TAG, "Search matched: '%s' by '%s' (%s)",
             outTrack.title.c_str(), outTrack.author.c_str(), outTrack.videoId.c_str());
    return ESP_OK;
}

esp_err_t InvidiousClient::resolveOpusUrl(const std::string& videoId, std::string& outUrl) {
    if (videoId.empty()) return ESP_ERR_INVALID_ARG;

    const std::string path = "/api/v1/videos/" + urlEncode(videoId) +
                             "?fields=adaptiveFormats(type,url,bitrate,qualityLabel)";

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

    JsonDocument doc;
    DeserializationError jsonErr = deserializeJson(doc, response, DeserializationOption::Filter(filter));
    if (jsonErr) {
        ESP_LOGE(TAG, "Video format JSON deserialize failed: %s", jsonErr.c_str());
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

    if (!bestUrl.empty()) {
        outUrl = bestUrl;
        ESP_LOGI(TAG, "Resolved stream URL (bitrate=%d, length=%zu)", bestBitrate, outUrl.length());
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to resolve usable audio format for %s", videoId.c_str());
    return ESP_ERR_NOT_FOUND;
}

esp_err_t InvidiousClient::getRecommendedTrack(const std::string& currentVideoId, InvidiousTrack& outTrack) {
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

    JsonObject first = recs[0];
    const char* vid = first["videoId"];
    const char* title = first["title"];
    const char* author = first["author"];
    int duration = first["lengthSeconds"] | 0;

    if (!vid || !title) return ESP_ERR_NOT_FOUND;

    outTrack.videoId = vid;
    outTrack.title = title;
    outTrack.author = author ? author : "";
    outTrack.durationSeconds = duration;

    ESP_LOGI(TAG, "Autoplay recommended: '%s' by '%s' (%s)",
             outTrack.title.c_str(), outTrack.author.c_str(), outTrack.videoId.c_str());
    return ESP_OK;
}
