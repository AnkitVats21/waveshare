#pragma once
#include "FreeRTOS.h"
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <cstdlib>

typedef enum {
    RINGBUF_TYPE_NOSPLIT = 0,
    RINGBUF_TYPE_ALLOWSPLIT = 1,
    RINGBUF_TYPE_BYTEBUF = 2
} RingbufferType_t;

struct MockRingbuffer {
    RingbufferType_t type;
    size_t capacity;
    std::vector<uint8_t> buffer;
    size_t head = 0; // write index
    size_t tail = 0; // read index
    size_t count = 0; // used bytes
    std::mutex mtx;
    std::condition_variable cv_read;
    std::condition_variable cv_write;

    MockRingbuffer(size_t cap, RingbufferType_t t) : type(t), capacity(cap), buffer(cap) {}

    size_t getFreeBytes() {
        std::lock_guard<std::mutex> lock(mtx);
        return capacity - count;
    }

    BaseType_t send(const void* data, size_t size, TickType_t timeout) {
        if (size == 0) return pdTRUE;
        std::unique_lock<std::mutex> lock(mtx);

        auto wait_pred = [this, size]() {
            if (type == RINGBUF_TYPE_BYTEBUF) {
                return (capacity - count) >= size;
            } else {
                return (capacity - count) >= (size + sizeof(uint32_t));
            }
        };

        if (!wait_pred()) {
            if (timeout == 0) return pdFALSE;
            if (timeout == portMAX_DELAY) {
                cv_write.wait(lock, wait_pred);
            } else {
                bool ok = cv_write.wait_for(lock, std::chrono::milliseconds(timeout), wait_pred);
                if (!ok) return pdFALSE;
            }
        }

        if (type == RINGBUF_TYPE_BYTEBUF) {
            const uint8_t* src = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; ++i) {
                buffer[head] = src[i];
                head = (head + 1) % capacity;
            }
            count += size;
        } else {
            // Write 4-byte length prefix then payload
            uint32_t len = static_cast<uint32_t>(size);
            const uint8_t* len_bytes = reinterpret_cast<const uint8_t*>(&len);
            for (size_t i = 0; i < sizeof(uint32_t); ++i) {
                buffer[head] = len_bytes[i];
                head = (head + 1) % capacity;
            }
            const uint8_t* src = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; ++i) {
                buffer[head] = src[i];
                head = (head + 1) % capacity;
            }
            count += (size + sizeof(uint32_t));
        }

        cv_read.notify_one();
        return pdTRUE;
    }

    void* receiveUpTo(size_t* bytes_out, TickType_t timeout, size_t max_bytes) {
        std::unique_lock<std::mutex> lock(mtx);

        auto wait_pred = [this]() { return count > 0; };
        if (!wait_pred()) {
            if (timeout == 0) {
                if (bytes_out) *bytes_out = 0;
                return nullptr;
            }
            if (timeout == portMAX_DELAY) {
                cv_read.wait(lock, wait_pred);
            } else {
                bool ok = cv_read.wait_for(lock, std::chrono::milliseconds(timeout), wait_pred);
                if (!ok) {
                    if (bytes_out) *bytes_out = 0;
                    return nullptr;
                }
            }
        }

        if (type == RINGBUF_TYPE_BYTEBUF) {
            size_t to_read = (max_bytes > 0 && max_bytes < count) ? max_bytes : count;
            uint8_t* out = static_cast<uint8_t*>(std::malloc(to_read));
            for (size_t i = 0; i < to_read; ++i) {
                out[i] = buffer[tail];
                tail = (tail + 1) % capacity;
            }
            count -= to_read;
            if (bytes_out) *bytes_out = to_read;
            cv_write.notify_one();
            return out;
        } else {
            // Read length header
            uint32_t len = 0;
            uint8_t* len_bytes = reinterpret_cast<uint8_t*>(&len);
            for (size_t i = 0; i < sizeof(uint32_t); ++i) {
                len_bytes[i] = buffer[tail];
                tail = (tail + 1) % capacity;
            }
            uint8_t* out = static_cast<uint8_t*>(std::malloc(len));
            for (size_t i = 0; i < len; ++i) {
                out[i] = buffer[tail];
                tail = (tail + 1) % capacity;
            }
            count -= (len + sizeof(uint32_t));
            if (bytes_out) *bytes_out = len;
            cv_write.notify_one();
            return out;
        }
    }
};

typedef MockRingbuffer* RingbufHandle_t;

inline RingbufHandle_t xRingbufferCreate(size_t size, RingbufferType_t type) {
    return new MockRingbuffer(size, type);
}

inline RingbufHandle_t xRingbufferCreateWithCaps(size_t size, RingbufferType_t type, uint32_t caps) {
    (void)caps;
    return new MockRingbuffer(size, type);
}

inline BaseType_t xRingbufferSend(RingbufHandle_t handle, const void* data, size_t size, TickType_t timeout) {
    if (!handle) return pdFALSE;
    return handle->send(data, size, timeout);
}

inline void* xRingbufferReceive(RingbufHandle_t handle, size_t* bytes_out, TickType_t timeout) {
    if (!handle) return nullptr;
    return handle->receiveUpTo(bytes_out, timeout, 0);
}

inline void* xRingbufferReceiveUpTo(RingbufHandle_t handle, size_t* bytes_out, TickType_t timeout, size_t max_bytes) {
    if (!handle) return nullptr;
    return handle->receiveUpTo(bytes_out, timeout, max_bytes);
}

inline void vRingbufferReturnItem(RingbufHandle_t handle, void* item) {
    (void)handle;
    if (item) std::free(item);
}

inline size_t xRingbufferGetCurFreeSize(RingbufHandle_t handle) {
    if (!handle) return 0;
    return handle->getFreeBytes();
}

inline void vRingbufferDelete(RingbufHandle_t handle) {
    delete handle;
}
