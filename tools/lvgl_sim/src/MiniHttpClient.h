#pragma once

#include <cstdint>
#include <string>

namespace lvgl_sim {

// Minimal blocking HTTP/1.1 client over a raw POSIX socket. Only handles
// what this tool needs: plain http:// GET/POST with a small JSON body and
// a small JSON response, single request per connection (no keep-alive).
class MiniHttpClient {
public:
    MiniHttpClient(std::string host, uint16_t port, int timeout_ms = 1500);

    // Returns true and fills out_body on HTTP 2xx. Returns false on any
    // connection/timeout/non-2xx failure.
    bool get(const std::string& path, std::string& out_body);
    bool post(const std::string& path, const std::string& body, std::string& out_body);

private:
    bool request(const std::string& method, const std::string& path,
                 const std::string& body, std::string& out_body);

    std::string m_host;
    uint16_t m_port;
    int m_timeout_ms;
};

} // namespace lvgl_sim
