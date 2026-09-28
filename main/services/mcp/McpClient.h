#pragma once

#include <string>
#include <mutex>

namespace Mcp {

class McpClient {
public:
    McpClient();
    ~McpClient();

    void setEndpoint(const std::string& url, const std::string& token);
    bool isConfigured() const;
    void resetSession();

    // Sends "initialize" and "notifications/initialized"
    bool initialize(std::string* error_out = nullptr);

    // Sends "tools/list" request. On 404, automatically re-initializes and retries once.
    bool listTools(std::string& raw_tools_response, std::string* error_out = nullptr);

    // Sends "tools/call" request. On 404, automatically re-initializes and retries once.
    bool callTool(const std::string& name, const std::string& arguments_json,
                  std::string& raw_call_response, std::string* error_out = nullptr);

    std::string currentUrl() const;
    std::string currentSessionId() const;
    bool hasToken() const;

private:
    int postJsonRpc(const std::string& payload, std::string& response_body, int expected_id, std::string* error_out);

    mutable std::mutex m_mutex;
    std::string m_url;
    std::string m_token;
    std::string m_session_id;
    bool m_initialized = false;
    int m_next_id = 1;
};

} // namespace Mcp
