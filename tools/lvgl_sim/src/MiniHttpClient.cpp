#include "MiniHttpClient.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace lvgl_sim {

namespace {

bool containsHeaderChunked(const std::string& headers) {
    std::string lower = headers;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower.find("transfer-encoding: chunked") != std::string::npos;
}

// Decodes an HTTP/1.1 chunked-transfer body. `body` is everything after the
// header block, already accumulated (connection was closed by the server).
std::string dechunk(const std::string& body) {
    std::string out;
    size_t pos = 0;
    while (pos < body.size()) {
        const size_t line_end = body.find("\r\n", pos);
        if (line_end == std::string::npos) break;
        std::string size_str = body.substr(pos, line_end - pos);
        const size_t semi = size_str.find(';');
        if (semi != std::string::npos) size_str.resize(semi);
        const unsigned long chunk_size = std::strtoul(size_str.c_str(), nullptr, 16);
        pos = line_end + 2;
        if (chunk_size == 0) break;
        if (pos + chunk_size > body.size()) break;
        out.append(body, pos, chunk_size);
        pos += chunk_size + 2; // skip trailing CRLF after chunk data
    }
    return out;
}

} // namespace

MiniHttpClient::MiniHttpClient(std::string host, uint16_t port, int timeout_ms)
    : m_host(std::move(host)), m_port(port), m_timeout_ms(timeout_ms) {}

bool MiniHttpClient::get(const std::string& path, std::string& out_body) {
    return request("GET", path, "", out_body);
}

bool MiniHttpClient::post(const std::string& path, const std::string& body, std::string& out_body) {
    return request("POST", path, body, out_body);
}

bool MiniHttpClient::request(const std::string& method, const std::string& path,
                             const std::string& body, std::string& out_body) {
    out_body.clear();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port_str = std::to_string(m_port);
    if (getaddrinfo(m_host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
        return false;
    }

    int fd = -1;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;

        timeval tv{};
        tv.tv_sec = m_timeout_ms / 1000;
        tv.tv_usec = (m_timeout_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return false;

    std::string req = method + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + m_host + "\r\n";
    req += "Connection: close\r\n";
    if (!body.empty()) {
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;

    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t n = send(fd, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) {
            close(fd);
            return false;
        }
        sent += static_cast<size_t>(n);
    }

    std::string raw;
    char buf[4096];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        raw.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    if (raw.empty()) return false;

    const size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;

    const std::string status_line = raw.substr(0, raw.find("\r\n"));
    // "HTTP/1.1 200 OK"
    const size_t sp1 = status_line.find(' ');
    if (sp1 == std::string::npos) return false;
    const int status_code = std::atoi(status_line.c_str() + sp1 + 1);
    if (status_code < 200 || status_code >= 300) return false;

    const std::string headers = raw.substr(0, header_end);
    const std::string raw_body = raw.substr(header_end + 4);
    out_body = containsHeaderChunked(headers) ? dechunk(raw_body) : raw_body;
    return true;
}

} // namespace lvgl_sim
