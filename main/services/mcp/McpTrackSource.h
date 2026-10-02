#pragma once

#include "media_player/TrackSource.h"
#include <ArduinoJson.h>
#include <atomic>
#include <cstdint>

namespace Mcp {

/**
 * @brief Song lookups through the MCP server's device-only music tools
 * (nexus-mcp music_search / music_track), which the voice model never sees.
 *
 * The server's stream URLs are resolved from the board's network (its API is
 * invidious-daemon), so the board plays them as before. When MCP is not
 * configured or a call fails, the lookup goes to `fallback` (the board's own
 * Invidious client, if built), and MCP is skipped for RETRY_AFTER_MS so a
 * server that is down doesn't cost a timeout per song. "Not found" from the
 * server is an answer, not a failure.
 */
class McpTrackSource : public TrackSource {
public:
    explicit McpTrackSource(TrackSource* fallback) : m_fallback(fallback) {}

    esp_err_t search(const std::string& query, InvidiousTrack& outTrack) override;
    esp_err_t searchList(const std::string& query, std::vector<InvidiousTrack>& outTracks, size_t limit) override;
    esp_err_t resolveStream(const std::string& videoId, std::string& outUrl) override;
    esp_err_t resolveWithRecommendations(const std::string& videoId, std::string& outUrl,
                                         std::vector<InvidiousTrack>& outRecommendations, size_t recLimit) override;
    esp_err_t recommendations(const std::string& videoId, std::vector<InvidiousTrack>& outTracks,
                              size_t limit) override;

    // A lookup the server can't answer now goes to the fallback for this long.
    static constexpr int64_t RETRY_AFTER_MS = 60000;
    // music_track can run yt-dlp on the server (10 s measured cold; its own
    // limit is 25 s).
    static constexpr int TRACK_TIMEOUT_MS  = 27000;
    static constexpr int SEARCH_TIMEOUT_MS = 12000;

private:
    // ESP_OK with the structured result, ESP_ERR_NOT_FOUND, or ESP_FAIL when
    // the server can't be used (caller falls back).
    esp_err_t call(const char* tool, const JsonDocument& args, JsonDocument& out, int timeout_ms);
    bool useMcp() const;

    TrackSource* m_fallback;
    std::atomic<int64_t> m_mcp_down_until_ms{0};
};

} // namespace Mcp
