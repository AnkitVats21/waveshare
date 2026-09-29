#include "services/mcp/McpService.h"
#include "services/storage/SystemDatabase.h"
#include "credentials/Credentials.h"
#include "common/thread_config.h"
#include "gemini_live/GeminiProtocol.h"
#include "gemini_live/PsramAllocator.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <ctime>

namespace Mcp {
namespace {

const char* TAG = "McpService";

struct McpCallJob {
    char call_id[64];
    std::string name;
    std::string args_json;
};

void mcpToolWorker(void* arg) {
    {
        auto* job = static_cast<McpCallJob*>(arg);
        int64_t t0 = esp_timer_get_time();
        std::string raw_resp;
        std::string err;
        bool ok = McpService::instance().client().callTool(job->name, job->args_json, raw_resp, &err);
        std::string gemini_formatted;
        if (!ok) {
            ESP_LOGW(TAG, "MCP tool '%s' call failed: %s", job->name.c_str(), err.c_str());
            JsonDocument errDoc;
            errDoc["status"] = "error";
            errDoc["message"] = "Remote tool call failed: " + (err.empty() ? "unknown error" : err);
            serializeJson(errDoc, gemini_formatted);
        } else {
            gemini_formatted = Mcp::formatToolResponseForGemini(raw_resp);
        }

        ESP_LOGI(TAG, "Tool '%s' completed in %lld ms (stack left %u B): %.200s",
                 job->name.c_str(), (esp_timer_get_time() - t0) / 1000,
                 (unsigned)uxTaskGetStackHighWaterMark(nullptr), gemini_formatted.c_str());

        GeminiProtocol::getInstance().transmitToolResponse(job->call_id, gemini_formatted.c_str());
        delete job;
    }
    vTaskDeleteWithCaps(nullptr);
}

void mcpRefreshWorker(void* arg) {
    {
        auto* self = static_cast<McpService*>(arg);
        std::string err;
        int64_t t0 = esp_timer_get_time();
        bool ok = self->refreshSync(&err);
        ESP_LOGI(TAG, "MCP refresh completed in %lld ms: %s (%zu tools)",
                 (esp_timer_get_time() - t0) / 1000, ok ? "OK" : err.c_str(),
                 self->toolCount());
    }
    vTaskDeleteWithCaps(nullptr);
}

} // namespace

McpService& McpService::instance() {
    static McpService s_instance;
    return s_instance;
}

McpService::McpService() = default;
McpService::~McpService() = default;

void McpService::start() {
    auto settings = Services::loadSettings();
    std::string token = credentials::mcpToken();

    m_max_tools = settings.mcp_max_tools > 0 ? settings.mcp_max_tools : 32;
    m_client.setEndpoint(settings.mcp_url, token);

    // Periodic refresh every 30 minutes
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = [](void* arg) {
        static_cast<McpService*>(arg)->refreshAsync();
    };
    timer_args.arg = this;
    timer_args.name = "mcp_refresh";
    if (esp_timer_create(&timer_args, &m_periodic_timer) == ESP_OK) {
        esp_timer_start_periodic(m_periodic_timer, 30ULL * 60 * 1000 * 1000);
    }

    if (isConfigured()) {
        ESP_LOGI(TAG, "MCP service started for endpoint: %s (max_tools: %u)",
                 settings.mcp_url.c_str(), m_max_tools);
    }
}

void McpService::onWifiConnected() {
    if (isConfigured()) {
        ESP_LOGI(TAG, "WiFi connected; refreshing MCP tools");
        refreshAsync();
    }
}

void McpService::refreshAsync() {
    if (!isConfigured()) return;
    if (m_refreshing.exchange(true)) {
        ESP_LOGD(TAG, "MCP refresh already running");
        return;
    }

    constexpr uint32_t STACK_SIZE = 8 * 1024;
    if (xTaskCreatePinnedToCoreWithCaps(mcpRefreshWorker, "mcp_ref", STACK_SIZE, this,
                                        ThreadConfig::LOW, nullptr,
                                        ThreadConfig::CORE_NETWORK, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "Failed to start MCP refresh task");
        m_refreshing = false;
    }
}

bool McpService::refreshSync(std::string* error_out) {
    struct RefreshGuard {
        std::atomic<bool>& flag;
        ~RefreshGuard() { flag = false; }
    } guard{m_refreshing};

    if (!isConfigured()) {
        if (error_out) *error_out = "MCP URL not configured";
        return false;
    }

    std::string raw_tools;
    std::string err;
    bool ok = m_client.listTools(raw_tools, &err);
    if (!ok) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connected = false;
        m_last_error = err.empty() ? "Failed to list tools" : err;
        if (error_out) *error_out = m_last_error;
        return false;
    }

    auto converted = Mcp::convertToolsListToDeclarations(raw_tools, m_max_tools);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tools.clear();
        for (const auto& t : converted.tools) {
            m_tools.push_back({PsramString(t.name.c_str()), PsramString(t.description.c_str()),
                               PsramString(t.declaration_json.c_str())});
        }
        m_connected = true;
        m_last_error.clear();
        m_last_refresh_epoch = time(nullptr);
    }
    return true;
}

bool McpService::isConfigured() const {
    return m_client.isConfigured();
}

bool McpService::isConnected() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connected;
}

std::string McpService::lastError() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_error;
}

int64_t McpService::lastRefreshEpoch() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_refresh_epoch;
}

size_t McpService::toolCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tools.size();
}

std::string McpService::url() const {
    return m_client.currentUrl();
}

uint8_t McpService::maxTools() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_max_tools;
}

bool McpService::hasToken() const {
    return m_client.hasToken();
}

std::vector<ConvertedTool> McpService::tools() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ConvertedTool> out;
    for (const auto& t : m_tools) out.push_back({t.name.c_str(), t.description.c_str(), ""});
    return out;
}

bool McpService::hasTool(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& t : m_tools) {
        if (name == t.name.c_str()) return true;
    }
    return false;
}

bool McpService::executeToolAsync(const char* call_id, const std::string& name, const std::string& args_json) {
    if (!hasTool(name)) {
        return false;
    }

    auto* job = new McpCallJob{};
    strncpy(job->call_id, call_id, sizeof(job->call_id) - 1);
    job->name = name;
    job->args_json = args_json;

    constexpr uint32_t STACK_SIZE = 8 * 1024;
    if (xTaskCreatePinnedToCoreWithCaps(mcpToolWorker, "mcp_tool", STACK_SIZE, job,
                                        ThreadConfig::NORMAL, nullptr,
                                        ThreadConfig::CORE_NETWORK, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "Failed to start MCP tool worker task");
        delete job;
        return false;
    }
    return true;
}

void McpService::populateGeminiDeclarations(JsonArray& functionDeclarations, const char* skip) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_tools.empty()) return;

    // Inserted as raw JSON (validated when cached): no parsing or deep copy
    // on the websocket task that sends the setup.
    for (const auto& t : m_tools) {
        if (skip && t.name == skip) continue;
        // A std::string is always copied into the document, so a refresh
        // meanwhile can't leave it pointing at freed text.
        functionDeclarations.add(serialized(std::string(t.declaration_json.c_str(), t.declaration_json.size())));
    }
}

void McpService::updateConfig(const std::string& url, uint8_t max_tools, const std::string& token) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_max_tools = max_tools > 0 ? max_tools : 32;
    }

    auto settings = Services::loadSettings();
    settings.mcp_url = url;
    settings.mcp_max_tools = m_max_tools;
    Services::saveSettings(settings, ndb::system::Settings::F_MCP_URL | ndb::system::Settings::F_MCP_MAX_TOOLS);

    credentials::setMcpToken(token);
    m_client.setEndpoint(url, token);

    refreshAsync();
}

} // namespace Mcp
