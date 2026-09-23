#include "services/http/FlashUpload.h"
#include "common/thread_config.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>

namespace FlashUpload {

namespace {

constexpr const char* TAG = "FlashUpload";

// Static TCB, stack and chunk buffer in internal SRAM (.bss), so the worker
// passes the cache-frozen stack check without a heap allocation.
StaticTask_t s_tcb;
StackType_t  s_stack[3072]; // 12 KB: esp_image validation + SHA-256
char         s_chunk[4096]; // one flash sector per write
std::atomic<bool> s_busy{false};

// Worker task that runs one call at a time on the static internal stack.
// Lives for the duration of one session (upload or single job).
class Worker {
public:
    bool start() {
        if (s_busy.exchange(true)) return false;
        m_req = xSemaphoreCreateBinary();
        m_done = xSemaphoreCreateBinary();
        m_running = true;
        if (m_req && m_done) {
            m_task = xTaskCreateStatic(loop, "flash_worker", sizeof(s_stack) / sizeof(s_stack[0]),
                                       this, ThreadConfig::Priority::LOW + 1, s_stack, &s_tcb);
        }
        if (!m_task) {
            stop();
            return false;
        }
        return true;
    }

    esp_err_t call(esp_err_t (*fn)(void*), void* ctx) {
        m_fn = fn;
        m_ctx = ctx;
        xSemaphoreGive(m_req);
        xSemaphoreTake(m_done, portMAX_DELAY);
        return m_result;
    }

    void stop() {
        if (m_task) {
            m_running = false;
            xSemaphoreGive(m_req);
            xSemaphoreTake(m_done, portMAX_DELAY); // worker has left its loop
            // Let the deletion finish before the static TCB/stack can be reused.
            vTaskDelay(pdMS_TO_TICKS(50));
            m_task = nullptr;
        }
        if (m_req) vSemaphoreDelete(m_req);
        if (m_done) vSemaphoreDelete(m_done);
        m_req = m_done = nullptr;
        s_busy = false;
    }

    // True if start() failed because another session is active.
    static bool busy() { return s_busy.load(); }

private:
    static void loop(void* arg) {
        auto* self = static_cast<Worker*>(arg);
        while (true) {
            xSemaphoreTake(self->m_req, portMAX_DELAY);
            if (!self->m_running) break;
            self->m_result = self->m_fn(self->m_ctx);
            xSemaphoreGive(self->m_done);
        }
        xSemaphoreGive(self->m_done);
        vTaskDelete(nullptr);
    }

    TaskHandle_t m_task = nullptr;
    SemaphoreHandle_t m_req = nullptr;
    SemaphoreHandle_t m_done = nullptr;
    volatile bool m_running = false;
    esp_err_t (*m_fn)(void*) = nullptr;
    void* m_ctx = nullptr;
    esp_err_t m_result = ESP_OK;
};

struct SinkCall {
    Sink* sink;
    size_t length;
};

esp_err_t sinkBegin(void* p)  { auto* c = static_cast<SinkCall*>(p); return c->sink->begin(c->length); }
esp_err_t sinkWrite(void* p)  { auto* c = static_cast<SinkCall*>(p); return c->sink->write(s_chunk, c->length); }
esp_err_t sinkFinish(void* p) { return static_cast<SinkCall*>(p)->sink->finish(); }
esp_err_t sinkAbort(void* p)  { static_cast<SinkCall*>(p)->sink->abort(); return ESP_OK; }

} // namespace

Result receive(httpd_req_t* req, Sink& sink, esp_err_t* err_out) {
    esp_err_t dummy;
    esp_err_t& err = err_out ? *err_out : dummy;
    err = ESP_OK;

    Worker worker;
    if (!worker.start()) {
        return Worker::busy() ? Result::BUSY : Result::WORKER_FAILED;
    }

    SinkCall call{ &sink, req->content_len };
    Result result = Result::OK;
    if ((err = worker.call(sinkBegin, &call)) != ESP_OK) {
        result = Result::BEGIN_FAILED;
    } else {
        size_t remaining = req->content_len;
        while (remaining > 0) {
            size_t to_read = remaining < sizeof(s_chunk) ? remaining : sizeof(s_chunk);
            int received = httpd_req_recv(req, s_chunk, to_read);
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (received <= 0) {
                ESP_LOGE(TAG, "Socket transfer interrupted (%u bytes left)", (unsigned)remaining);
                result = Result::RECV_FAILED;
                break;
            }
            call.length = received;
            if ((err = worker.call(sinkWrite, &call)) != ESP_OK) {
                result = Result::WRITE_FAILED;
                break;
            }
            remaining -= received;
        }
        if (result == Result::OK && (err = worker.call(sinkFinish, &call)) != ESP_OK) {
            result = Result::FINISH_FAILED;
        }
        if (result != Result::OK) {
            worker.call(sinkAbort, &call);
        }
    }
    worker.stop();
    return result;
}

esp_err_t runInternal(esp_err_t (*fn)(void*), void* ctx) {
    Worker worker;
    if (!worker.start()) {
        return Worker::busy() ? ESP_ERR_INVALID_STATE : ESP_ERR_NO_MEM;
    }
    esp_err_t err = worker.call(fn, ctx);
    worker.stop();
    return err;
}

} // namespace FlashUpload
