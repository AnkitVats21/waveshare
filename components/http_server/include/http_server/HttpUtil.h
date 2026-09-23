#pragma once

#include "esp_http_server.h"
#include <ArduinoJson.h>
#include <string>

/**
 * @brief Request/response helpers shared by all route modules.
 *
 * JSON responses carry permissive CORS headers and `Connection: close`, so a
 * dashboard served from another origin (dev server) can call the API.
 */
namespace Http {

void urlDecode(const std::string& in, std::string& out);

// URL-decoded query parameter; false if absent.
bool queryParam(httpd_req_t* req, const char* name, std::string& out);

// Reads the whole body (looping over partial receives). False if the body is
// empty, longer than max_len, or the socket fails.
bool readBody(httpd_req_t* req, std::string& out, size_t max_len);

void setCorsHeaders(httpd_req_t* req);

esp_err_t sendJson(httpd_req_t* req, int status, const std::string& json);
esp_err_t sendJson(httpd_req_t* req, int status, const JsonDocument& doc);

// {"status":"error","message":...}
esp_err_t sendError(httpd_req_t* req, int status, const char* message);

// {"status":"ok","message":...}
esp_err_t sendOk(httpd_req_t* req, const char* message);

// Empty 200 with CORS headers, for OPTIONS preflight.
esp_err_t corsPreflight(httpd_req_t* req);

} // namespace Http
