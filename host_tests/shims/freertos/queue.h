#pragma once
#include "FreeRTOS.h"
#include <queue>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstring>

struct MockQueue {
    size_t item_size;
    size_t max_items;
    std::queue<std::vector<uint8_t>> items;
    std::mutex mtx;
    std::condition_variable cv;

    MockQueue(size_t len, size_t sz) : item_size(sz), max_items(len) {}
};

typedef MockQueue* QueueHandle_t;

inline QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size) {
    return new MockQueue(len, item_size);
}

inline BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t timeout) {
    if (!q) return pdFALSE;
    std::unique_lock<std::mutex> lock(q->mtx);
    if (q->items.size() >= q->max_items) {
        if (timeout == 0) return pdFALSE;
    }
    const uint8_t* p = static_cast<const uint8_t*>(item);
    q->items.push(std::vector<uint8_t>(p, p + q->item_size));
    q->cv.notify_one();
    return pdTRUE;
}

inline BaseType_t xQueueReceive(QueueHandle_t q, void* buffer, TickType_t timeout) {
    if (!q) return pdFALSE;
    std::unique_lock<std::mutex> lock(q->mtx);
    if (q->items.empty()) {
        if (timeout == 0) return pdFALSE;
        if (timeout == portMAX_DELAY) {
            q->cv.wait(lock, [q] { return !q->items.empty(); });
        } else {
            bool ok = q->cv.wait_for(lock, std::chrono::milliseconds(timeout), [q] { return !q->items.empty(); });
            if (!ok) return pdFALSE;
        }
    }
    if (q->items.empty()) return pdFALSE;
    auto item = q->items.front();
    q->items.pop();
    std::memcpy(buffer, item.data(), q->item_size);
    return pdTRUE;
}

inline void vQueueDelete(QueueHandle_t q) {
    delete q;
}
