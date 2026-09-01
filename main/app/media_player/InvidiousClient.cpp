#include "InvidiousClient.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
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

bool containsIgnoreCase(const char* value, const char* needle) {
    if (!value || !needle) return false;
    std::string haystack(value);
    std::string target(needle);
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(target.begin(), target.end(), target.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return haystack.find(target) != std::string::npos;
}

} // namespace

InvidiousClient::InvidiousClient(const char* instanceHost)
    : _instanceHost(instanceHost ? instanceHost : "") {}

esp_err_t InvidiousClient::httpEventHandler(void* event) {
    auto* evt = static_cast<esp_http_client_event_t*>(event);
    auto* response = static_cast<std::string*>(evt->user_data);

    if (evt->event_id == HTTP_EVENT_ON_DATA && response && evt->data && evt->data_len > 0) {
        response->append(static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

esp_err_t InvidiousClient::httpGet(const std::string& url, std::string& outResponse) {
    if (_instanceHost.empty() || url.empty()) return ESP_ERR_INVALID_ARG;

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = httpEventHandler;
    config.user_data = &outResponse;
    config.timeout_ms = 10000;
    config.buffer_size = 4096;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        const int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGE(TAG, "Invidious HTTP status: %d", status);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "Invidious request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

esp_err_t InvidiousClient::search(const std::string& query, InvidiousTrack& outTrack) {
    if (query.empty()) return ESP_ERR_INVALID_ARG;

    const std::string url = "https://" + _instanceHost +
        "/api/v1/search?q=" + urlEncode(query) + "&type=video";

    std::string response;
    esp_err_t err = httpGet(url, response);
    if (err != ESP_OK || response.empty()) return err != ESP_OK ? err : ESP_FAIL;

    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse search response");
        return ESP_FAIL;
    }

    cJSON* first = cJSON_GetArrayItem(root, 0);
    if (!first) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON* videoId = cJSON_GetObjectItemCaseSensitive(first, "videoId");
    cJSON* title = cJSON_GetObjectItemCaseSensitive(first, "title");
    cJSON* author = cJSON_GetObjectItemCaseSensitive(first, "author");
    cJSON* duration = cJSON_GetObjectItemCaseSensitive(first, "lengthSeconds");

    if (!cJSON_IsString(videoId) || !cJSON_IsString(title)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    outTrack.videoId = videoId->valuestring;
    outTrack.title = title->valuestring;
    outTrack.author = cJSON_IsString(author) ? author->valuestring : "";
    outTrack.durationSeconds = cJSON_IsNumber(duration) ? duration->valueint : 0;

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Search match: %s (%s)", outTrack.title.c_str(), outTrack.videoId.c_str());
    return ESP_OK;
}

esp_err_t InvidiousClient::resolveOpusUrl(const std::string& videoId, std::string& outUrl) {
    if (videoId.empty()) return ESP_ERR_INVALID_ARG;

    const std::string url = "https://" + _instanceHost +
        "/api/v1/videos/" + urlEncode(videoId);

    std::string response;
    esp_err_t err = httpGet(url, response);
    if (err != ESP_OK || response.empty()) return err != ESP_OK ? err : ESP_FAIL;

    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) return ESP_FAIL;

    cJSON* formats = cJSON_GetObjectItemCaseSensitive(root, "adaptiveFormats");
    if (!cJSON_IsArray(formats)) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    // Prefer audio-only Opus. Invidious may return several Opus variants;
    // select the highest bitrate among URL-bearing audio-only formats.
    int bestBitrate = -1;
    const char* bestUrl = nullptr;
    cJSON* format = nullptr;
    cJSON_ArrayForEach(format, formats) {
        cJSON* type = cJSON_GetObjectItemCaseSensitive(format, "type");
        cJSON* streamUrl = cJSON_GetObjectItemCaseSensitive(format, "url");
        cJSON* bitrate = cJSON_GetObjectItemCaseSensitive(format, "bitrate");
        cJSON* quality = cJSON_GetObjectItemCaseSensitive(format, "qualityLabel");

        if (!cJSON_IsString(type) || !cJSON_IsString(streamUrl)) continue;
        if (!containsIgnoreCase(type->valuestring, "audio/opus")) continue;
        if (cJSON_IsString(quality)) continue; // Defensive: don't select video-labelled formats.

        const int candidateBitrate = cJSON_IsNumber(bitrate) ? bitrate->valueint : 0;
        if (candidateBitrate > bestBitrate) {
            bestBitrate = candidateBitrate;
            bestUrl = streamUrl->valuestring;
        }
    }

    if (!bestUrl) {
        // Some instances use a MIME string containing codec parameters.
        cJSON_ArrayForEach(format, formats) {
            cJSON* type = cJSON_GetObjectItemCaseSensitive(format, "type");
            cJSON* streamUrl = cJSON_GetObjectItemCaseSensitive(format, "url");
            if (cJSON_IsString(type) && cJSON_IsString(streamUrl) &&
                containsIgnoreCase(type->valuestring, "opus")) {
                bestUrl = streamUrl->valuestring;
                break;
            }
        }
    }

    if (bestUrl) outUrl = bestUrl;
    cJSON_Delete(root);
    return bestUrl ? ESP_OK : ESP_ERR_NOT_FOUND;
}
