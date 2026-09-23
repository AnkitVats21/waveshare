#include "http_server/HttpUtil.h"
#include <cstdio>
#include <cstdlib>

namespace Http {

void urlDecode(const std::string& in, std::string& out) {
    out.clear();
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); ++i) {
        if (in[i] == '%') {
            if (i + 2 < in.length()) {
                char hex[3] = { in[i+1], in[i+2], '\0' };
                char* end = nullptr;
                long val = strtol(hex, &end, 16);
                if (end != hex) {
                    out += static_cast<char>(val);
                    i += 2;
                } else {
                    out += '%';
                }
            } else {
                out += '%';
            }
        } else if (in[i] == '+') {
            out += ' ';
        } else {
            out += in[i];
        }
    }
}

bool queryParam(httpd_req_t* req, const char* name, std::string& out) {
    out.clear();
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0) return false;

    std::string query(query_len + 1, '\0');
    if (httpd_req_get_url_query_str(req, &query[0], query_len + 1) != ESP_OK) return false;

    std::string value(query_len + 1, '\0');
    if (httpd_query_key_value(query.c_str(), name, &value[0], query_len + 1) != ESP_OK) return false;

    urlDecode(value.c_str(), out);
    return true;
}

bool readBody(httpd_req_t* req, std::string& out, size_t max_len) {
    out.clear();
    if (req->content_len == 0 || req->content_len > max_len) return false;

    out.resize(req->content_len);
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, &out[got], req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            out.clear();
            return false;
        }
        got += n;
    }
    return true;
}

void setCorsHeaders(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

static void setStatus(httpd_req_t* req, int status, char* buf, size_t buf_len) {
    switch (status) {
        case 200: httpd_resp_set_status(req, "200 OK"); break;
        case 400: httpd_resp_set_status(req, "400 Bad Request"); break;
        case 404: httpd_resp_set_status(req, "404 Not Found"); break;
        case 500: httpd_resp_set_status(req, "500 Internal Server Error"); break;
        default:
            // httpd keeps the pointer until the response is sent, so buf must
            // outlive this call.
            snprintf(buf, buf_len, "%d Status", status);
            httpd_resp_set_status(req, buf);
            break;
    }
}

esp_err_t sendJson(httpd_req_t* req, int status, const std::string& json) {
    setCorsHeaders(req);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_type(req, "application/json");
    char status_buf[32];
    setStatus(req, status, status_buf, sizeof(status_buf));
    return httpd_resp_send(req, json.c_str(), json.length());
}

esp_err_t sendJson(httpd_req_t* req, int status, const JsonDocument& doc) {
    std::string out;
    serializeJson(doc, out);
    return sendJson(req, status, out);
}

esp_err_t sendError(httpd_req_t* req, int status, const char* message) {
    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = message;
    return sendJson(req, status, doc);
}

esp_err_t sendOk(httpd_req_t* req, const char* message) {
    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = message;
    return sendJson(req, 200, doc);
}

esp_err_t corsPreflight(httpd_req_t* req) {
    setCorsHeaders(req);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_send(req, nullptr, 0);
}

} // namespace Http
