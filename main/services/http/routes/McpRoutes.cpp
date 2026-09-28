#include "services/http/routes/Routes.h"
#include "http_server/HttpUtil.h"
#include "services/mcp/McpService.h"
#include "services/mcp/McpUtil.h"
#include "credentials/Credentials.h"

#include <ArduinoJson.h>

namespace {

esp_err_t getMcpStatus(httpd_req_t* req) {
    auto& mcp = Mcp::McpService::instance();
    JsonDocument doc;

    doc["configured"] = mcp.isConfigured();
    doc["connected"] = mcp.isConnected();
    doc["url"] = mcp.url();
    doc["has_token"] = mcp.hasToken();
    doc["max_tools"] = mcp.maxTools();
    doc["tools_count"] = mcp.toolCount();
    doc["last_refresh_epoch"] = mcp.lastRefreshEpoch();
    doc["last_error"] = mcp.lastError();

    JsonArray toolsArray = doc["tools"].to<JsonArray>();
    auto tools = mcp.tools();
    for (const auto& t : tools) {
        JsonObject item = toolsArray.add<JsonObject>();
        item["name"] = t.name;
        item["description"] = t.description;
    }

    return Http::sendJson(req, 200, doc);
}

esp_err_t setMcpConfig(httpd_req_t* req) {
    std::string body;
    JsonDocument doc;
    if (req->content_len == 0 || !Http::readBody(req, body, 2048) || deserializeJson(doc, body)) {
        return Http::sendError(req, 400, "Invalid JSON body");
    }

    auto& mcp = Mcp::McpService::instance();
    std::string url = doc["url"] | mcp.url();
    uint8_t max_tools = doc["max_tools"] | mcp.maxTools();
    
    // Only update token if explicitly provided in body
    std::string token;
    bool update_token = false;
    if (doc["token"].is<const char*>()) {
        token = doc["token"].as<const char*>();
        update_token = true;
    }

    if (!url.empty()) {
        std::string err;
        if (!Mcp::isUrlAllowed(url, &err)) {
            return Http::sendError(req, 400, err.c_str());
        }
    }

    if (!update_token) {
        // Keep existing token
        token = credentials::mcpToken();
    }

    mcp.updateConfig(url, max_tools, token);
    return Http::sendOk(req, "MCP configuration updated");
}

esp_err_t refreshMcpTools(httpd_req_t* req) {
    auto& mcp = Mcp::McpService::instance();
    if (!mcp.isConfigured()) {
        return Http::sendError(req, 400, "MCP URL not configured");
    }

    mcp.refreshAsync();
    return Http::sendOk(req, "MCP refresh scheduled");
}

} // namespace

namespace Routes {

void registerMcp(Http::Server& server) {
    server.on("/api/mcp", HTTP_GET, getMcpStatus);
    server.on("/api/mcp", HTTP_POST, setMcpConfig);
    server.on("/api/mcp/refresh", HTTP_POST, refreshMcpTools);
}

} // namespace Routes
