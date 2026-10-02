#pragma once

#include "nexus_db/PsramAllocator.h"
#include "services/mcp/McpClient.h"
#include "services/mcp/McpUtil.h"

#include <ArduinoJson.h>
#include <esp_timer.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace Mcp {

class McpService {
public:
    static McpService& instance();

    void start();
    void onWifiConnected();

    void refreshAsync();
    bool refreshSync(std::string* error_out = nullptr);

    bool isConfigured() const;
    bool isConnected() const;
    std::string lastError() const;
    int64_t lastRefreshEpoch() const;
    size_t toolCount() const;
    std::string url() const;
    uint8_t maxTools() const;
    bool hasToken() const;

    std::vector<ConvertedTool> tools() const;
    bool hasTool(const std::string& name) const;

    bool executeToolAsync(const char* call_id, const std::string& name, const std::string& args_json);

    // Calls a device-only tool (one the server marks _meta "nexus/device_only")
    // and blocks for its structuredContent. False, with the reason, when MCP
    // is not configured, the call fails, or the tool reports an error.
    bool callDeviceTool(const std::string& name, const std::string& args_json, JsonDocument& out,
                        std::string* error = nullptr, int timeout_ms = McpClient::DEFAULT_TIMEOUT_MS);
    // skip: a tool name to leave out, or nullptr.
    void populateGeminiDeclarations(JsonArray& functionDeclarations, const char* skip = nullptr);

    void updateConfig(const std::string& url, uint8_t max_tools, const std::string& token);

    McpClient& client() { return m_client; }

private:
    McpService();
    ~McpService();
    McpService(const McpService&) = delete;
    McpService& operator=(const McpService&) = delete;

    mutable std::mutex m_mutex;
    McpClient m_client;
    uint8_t m_max_tools = 32;

    // The cache lives in PSRAM: allocations under 4 KB come from internal
    // RAM, and 32 declarations of a few hundred bytes each would take
    // 15-30 KB of it.
    using PsramString = std::basic_string<char, std::char_traits<char>, nexus_db::PsramAllocator<char>>;
    struct CachedTool {
        PsramString name;
        PsramString description;
        PsramString declaration_json;
    };
    std::vector<CachedTool, nexus_db::PsramAllocator<CachedTool>> m_tools;
    bool m_connected = false;
    std::string m_last_error;
    int64_t m_last_refresh_epoch = 0;

    std::atomic<bool> m_refreshing{false};
    esp_timer_handle_t m_periodic_timer = nullptr;
};

} // namespace Mcp
