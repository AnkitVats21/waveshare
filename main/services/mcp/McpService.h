#pragma once

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
    void populateGeminiDeclarations(JsonArray& functionDeclarations);

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

    std::vector<ConvertedTool> m_tools;
    bool m_connected = false;
    std::string m_last_error;
    int64_t m_last_refresh_epoch = 0;

    std::atomic<bool> m_refreshing{false};
    esp_timer_handle_t m_periodic_timer = nullptr;
};

} // namespace Mcp
