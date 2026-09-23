#pragma once

#include "esp_http_server.h"
#include <cstddef>

/**
 * @brief Flash access from the HTTP server.
 *
 * Flash writes, partition mmap and NVS writes freeze or disable the cache, so
 * they must run on a task whose stack is in internal RAM; the httpd task stack
 * is in PSRAM. Everything here runs on a dedicated worker task with a static
 * internal-RAM stack. One job/upload at a time.
 */
namespace FlashUpload {

class Sink {
public:
    virtual ~Sink() = default;
    virtual esp_err_t begin(size_t total_len) = 0;
    virtual esp_err_t write(const char* data, size_t len) = 0;
    virtual esp_err_t finish() = 0;  // verify + commit
    virtual void abort() = 0;
};

enum class Result {
    OK,
    BUSY,           // another upload/job is running
    WORKER_FAILED,  // could not start the worker task
    BEGIN_FAILED,
    RECV_FAILED,    // socket error mid-transfer
    WRITE_FAILED,
    FINISH_FAILED,
};

// Streams the whole request body into `sink` in 4 KB chunks (internal-RAM
// buffer). On any failure after begin() the sink is aborted. `err` receives
// the failing call's esp_err_t.
Result receive(httpd_req_t* req, Sink& sink, esp_err_t* err = nullptr);

// Runs fn(ctx) on the worker. ESP_ERR_INVALID_STATE if busy, ESP_ERR_NO_MEM if
// the worker can't start; otherwise fn's result.
esp_err_t runInternal(esp_err_t (*fn)(void*), void* ctx);

} // namespace FlashUpload
