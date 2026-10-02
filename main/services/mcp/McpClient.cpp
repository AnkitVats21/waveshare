#include "services/mcp/McpClient.h"
#include "services/mcp/McpUtil.h"
#include "media_player/TlsConfig.h"

#include <esp_http_client.h>
#include <esp_log.h>

#include <cstring>
#include <strings.h>

namespace Mcp {
namespace {

const char* TAG = "McpClient";
constexpr size_t MAX_RESPONSE_BODY = 48 * 1024;

struct ResponseContext {
    std::string raw_body;
    std::string captured_session_id;
};

esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
    auto* ctx = static_cast<ResponseContext*>(evt->user_data);
    if (!ctx) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (evt->header_key && evt->header_value) {
            if (strcasecmp(evt->header_key, "Mcp-Session-Id") == 0) {
                ctx->captured_session_id = evt->header_value;
            }
        }
    } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (evt->data && evt->data_len > 0) {
            if (ctx->raw_body.size() + evt->data_len <= MAX_RESPONSE_BODY) {
                ctx->raw_body.append(static_cast<const char*>(evt->data), evt->data_len);
            }
        }
    }
    return ESP_OK;
}

} // namespace

McpClient::McpClient() = default;
McpClient::~McpClient() = default;

void McpClient::setEndpoint(const std::string& url, const std::string& token) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_url != url || m_token != token) {
        m_url = url;
        m_token = token;
        m_session_id.clear();
        m_initialized = false;
        m_next_id = 1;
    }
}

bool McpClient::isConfigured() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_url.empty();
}

void McpClient::resetSession() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_session_id.clear();
    m_initialized = false;
}

std::string McpClient::currentUrl() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_url;
}

std::string McpClient::currentSessionId() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_session_id;
}

bool McpClient::hasToken() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_token.empty();
}

int McpClient::postJsonRpc(const std::string& payload, std::string& response_body, int expected_id, std::string* error_out,
                           int timeout_ms) {
    if (m_url.empty()) {
        if (error_out) *error_out = "MCP URL not configured";
        return -1;
    }

    std::string url_err;
    if (!isUrlAllowed(m_url, &url_err)) {
        ESP_LOGW(TAG, "URL blocked by security policy: %s", url_err.c_str());
        if (error_out) *error_out = url_err;
        return -1;
    }

    ResponseContext ctx;
    esp_http_client_config_t config = {};
    config.url = m_url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = timeout_ms;
    config.buffer_size = 4096;
    config.event_handler = httpEventHandler;
    config.user_data = &ctx;

    if (m_url.rfind("https://", 0) == 0) {
        Tls::secure(config);
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        if (error_out) *error_out = "Failed to initialize HTTP client";
        return -1;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json, text/event-stream");
    esp_http_client_set_header(client, "MCP-Protocol-Version", "2024-11-05");

    if (!m_token.empty()) {
        std::string auth = "Bearer " + m_token;
        esp_http_client_set_header(client, "Authorization", auth.c_str());
    }

    if (!m_session_id.empty()) {
        esp_http_client_set_header(client, "Mcp-Session-Id", m_session_id.c_str());
    }

    esp_http_client_set_post_field(client, payload.data(), payload.size());

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (!ctx.captured_session_id.empty()) {
        m_session_id = ctx.captured_session_id;
    }

    if (err != ESP_OK) {
        std::string err_str = esp_err_to_name(err);
        ESP_LOGW(TAG, "POST failed: %s", err_str.c_str());
        if (error_out) *error_out = err_str;
        return -1;
    }

    if (status >= 200 && status < 300) {
        response_body = extractJsonRpcBody(ctx.raw_body, expected_id);
        return status;
    }

    if (status == 404) {
        if (error_out) *error_out = "Session not found (HTTP 404)";
        return 404;
    }

    std::string msg = "HTTP " + std::to_string(status);
    ESP_LOGW(TAG, "Server returned status: %d", status);
    if (error_out) *error_out = msg;
    return status;
}

bool McpClient::initialize(std::string* error_out) {
    if (m_url.empty()) {
        if (error_out) *error_out = "MCP URL not configured";
        return false;
    }

    m_session_id.clear();
    m_initialized = false;

    int id = m_next_id++;
    std::string init_payload =
        "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
        ",\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\","
        "\"capabilities\":{},\"clientInfo\":{\"name\":\"nexus-firmware\",\"version\":\"1.0.0\"}}}";

    std::string resp;
    int status = postJsonRpc(init_payload, resp, id, error_out);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "initialize RPC failed: status %d", status);
        return false;
    }

    m_initialized = true;

    // Send notifications/initialized notification
    std::string notif_payload = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    std::string dummy_resp;
    postJsonRpc(notif_payload, dummy_resp, -1, nullptr);

    return true;
}

bool McpClient::listTools(std::string& raw_tools_response, std::string* error_out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_url.empty()) {
        if (error_out) *error_out = "MCP URL not configured";
        return false;
    }

    if (!m_initialized) {
        if (!initialize(error_out)) {
            return false;
        }
    }

    int id = m_next_id++;
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"method\":\"tools/list\",\"params\":{}}";
    int status = postJsonRpc(payload, raw_tools_response, id, error_out);

    if (status == 404) {
        ESP_LOGI(TAG, "Session 404 on tools/list; re-initializing session");
        if (initialize(error_out)) {
            id = m_next_id++;
            payload = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"method\":\"tools/list\",\"params\":{}}";
            status = postJsonRpc(payload, raw_tools_response, id, error_out);
        }
    }

    return (status >= 200 && status < 300 && !raw_tools_response.empty());
}

bool McpClient::callTool(const std::string& name, const std::string& arguments_json,
                         std::string& raw_call_response, std::string* error_out, int timeout_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_url.empty()) {
        if (error_out) *error_out = "MCP URL not configured";
        return false;
    }

    if (!m_initialized) {
        if (!initialize(error_out)) {
            return false;
        }
    }

    auto makePayload = [&](int call_rpc_id) {
        std::string p = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(call_rpc_id) +
                        ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + name + "\",\"arguments\":";
        if (arguments_json.empty()) {
            p += "{}";
        } else {
            p += arguments_json;
        }
        p += "}}";
        return p;
    };

    int id = m_next_id++;
    std::string payload = makePayload(id);
    int status = postJsonRpc(payload, raw_call_response, id, error_out, timeout_ms);

    if (status == 404) {
        ESP_LOGI(TAG, "Session 404 on tools/call; re-initializing session");
        if (initialize(error_out)) {
            id = m_next_id++;
            payload = makePayload(id);
            status = postJsonRpc(payload, raw_call_response, id, error_out, timeout_ms);
        }
    }

    return (status >= 200 && status < 300 && !raw_call_response.empty());
}

} // namespace Mcp
