#include "services/mcp/McpTrackSource.h"
#include "services/mcp/McpService.h"
#include <esp_log.h>
#include <esp_timer.h>

namespace Mcp {

namespace {

constexpr const char* TAG = "McpTracks";

int64_t nowMs() { return esp_timer_get_time() / 1000; }

InvidiousTrack toTrack(JsonVariantConst t) {
    InvidiousTrack out;
    out.videoId = t["id"] | "";
    out.title = t["title"] | "";
    out.author = t["artist"] | "";
    out.durationSeconds = t["duration"] | 0;
    return out;
}

void toTracks(JsonVariantConst list, std::vector<InvidiousTrack>& out) {
    out.clear();
    for (JsonVariantConst t : list.as<JsonArrayConst>()) {
        InvidiousTrack track = toTrack(t);
        if (!track.videoId.empty()) out.push_back(std::move(track));
    }
}

}  // namespace

bool McpTrackSource::useMcp() const {
    return McpService::instance().isConfigured() && nowMs() >= m_mcp_down_until_ms.load();
}

esp_err_t McpTrackSource::call(const char* tool, const JsonDocument& args, JsonDocument& out, int timeout_ms) {
    std::string argsJson;
    serializeJson(args, argsJson);
    std::string err;
    if (!McpService::instance().callDeviceTool(tool, argsJson, out, &err, timeout_ms)) {
        ESP_LOGW(TAG, "%s failed (%s)%s", tool, err.c_str(),
                 m_fallback ? "; Invidious fallback for the next minute" : "");
        m_mcp_down_until_ms = nowMs() + RETRY_AFTER_MS;
        return ESP_FAIL;
    }
    if (strcmp(out["error"] | "", "not_found") == 0) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

esp_err_t McpTrackSource::search(const std::string& query, InvidiousTrack& outTrack) {
    std::vector<InvidiousTrack> tracks;
    esp_err_t err = searchList(query, tracks, 1);
    if (err != ESP_OK) return err;
    if (tracks.empty()) return ESP_ERR_NOT_FOUND;
    outTrack = tracks[0];
    return ESP_OK;
}

esp_err_t McpTrackSource::searchList(const std::string& query, std::vector<InvidiousTrack>& outTracks, size_t limit) {
    if (useMcp()) {
        JsonDocument args, out;
        args["query"] = query;
        args["limit"] = limit;
        esp_err_t err = call("music_search", args, out, SEARCH_TIMEOUT_MS);
        if (err == ESP_OK) {
            toTracks(out["tracks"], outTracks);
            return outTracks.empty() ? ESP_ERR_NOT_FOUND : ESP_OK;
        }
        if (err == ESP_ERR_NOT_FOUND) return err;
    }
    return m_fallback ? m_fallback->searchList(query, outTracks, limit) : ESP_FAIL;
}

esp_err_t McpTrackSource::resolveStream(const std::string& videoId, std::string& outUrl) {
    std::vector<InvidiousTrack> none;
    return resolveWithRecommendations(videoId, outUrl, none, 0);
}

esp_err_t McpTrackSource::resolveWithRecommendations(const std::string& videoId, std::string& outUrl,
                                                     std::vector<InvidiousTrack>& outRecommendations,
                                                     size_t recLimit) {
    if (useMcp()) {
        JsonDocument args, out;
        args["id"] = videoId;
        args["recommendations"] = recLimit;
        esp_err_t err = call("music_track", args, out, TRACK_TIMEOUT_MS);
        if (err == ESP_OK) {
            outUrl = out["stream_url"] | "";
            toTracks(out["recommendations"], outRecommendations);
            return outUrl.empty() ? ESP_ERR_NOT_FOUND : ESP_OK;
        }
        if (err == ESP_ERR_NOT_FOUND) return err;
    }
    if (!m_fallback) return ESP_FAIL;
    return recLimit ? m_fallback->resolveWithRecommendations(videoId, outUrl, outRecommendations, recLimit)
                    : m_fallback->resolveStream(videoId, outUrl);
}

esp_err_t McpTrackSource::recommendations(const std::string& videoId, std::vector<InvidiousTrack>& outTracks,
                                          size_t limit) {
    if (useMcp()) {
        JsonDocument args, out;
        args["id"] = videoId;
        args["stream"] = false;
        args["recommendations"] = limit;
        esp_err_t err = call("music_track", args, out, TRACK_TIMEOUT_MS);
        if (err == ESP_OK) {
            toTracks(out["recommendations"], outTracks);
            return ESP_OK;
        }
        if (err == ESP_ERR_NOT_FOUND) return err;
    }
    return m_fallback ? m_fallback->recommendations(videoId, outTracks, limit) : ESP_FAIL;
}

} // namespace Mcp
