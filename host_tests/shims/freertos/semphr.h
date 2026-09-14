#pragma once
#include "FreeRTOS.h"
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>

struct SemaphoreBase {
    virtual ~SemaphoreBase() = default;
    virtual BaseType_t take(TickType_t timeout) = 0;
    virtual BaseType_t give() = 0;
    virtual BaseType_t takeRecursive(TickType_t timeout) { return take(timeout); }
    virtual BaseType_t giveRecursive() { return give(); }
};

struct CountingSemaphoreMock : public SemaphoreBase {
    std::mutex mtx;
    std::condition_variable cv;
    int count;
    const int max_count;

    CountingSemaphoreMock(int max, int initial) : count(initial), max_count(max) {}

    BaseType_t take(TickType_t timeout) override {
        std::unique_lock<std::mutex> lock(mtx);
        if (count > 0) {
            --count;
            return pdTRUE;
        }
        if (timeout == 0) {
            return pdFALSE;
        }
        if (timeout == portMAX_DELAY) {
            cv.wait(lock, [this] { return count > 0; });
            --count;
            return pdTRUE;
        }
        bool ok = cv.wait_for(lock, std::chrono::milliseconds(timeout), [this] { return count > 0; });
        if (ok) {
            --count;
            return pdTRUE;
        }
        return pdFALSE;
    }

    BaseType_t give() override {
        std::lock_guard<std::mutex> lock(mtx);
        if (count < max_count) {
            ++count;
            cv.notify_one();
            return pdTRUE;
        }
        return pdFALSE;
    }
};

struct MutexMock : public SemaphoreBase {
    std::mutex mtx;

    BaseType_t take(TickType_t timeout) override {
        if (timeout == 0) {
            return mtx.try_lock() ? pdTRUE : pdFALSE;
        }
        if (timeout == portMAX_DELAY) {
            mtx.lock();
            return pdTRUE;
        }
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(timeout)) {
            if (mtx.try_lock()) return pdTRUE;
            std::this_thread::yield();
        }
        return mtx.try_lock() ? pdTRUE : pdFALSE;
    }

    BaseType_t give() override {
        mtx.unlock();
        return pdTRUE;
    }
};

struct RecursiveMutexMock : public SemaphoreBase {
    std::recursive_mutex mtx;

    BaseType_t take(TickType_t timeout) override {
        return takeRecursive(timeout);
    }
    BaseType_t give() override {
        return giveRecursive();
    }
    BaseType_t takeRecursive(TickType_t timeout) override {
        if (timeout == 0) {
            return mtx.try_lock() ? pdTRUE : pdFALSE;
        }
        if (timeout == portMAX_DELAY) {
            mtx.lock();
            return pdTRUE;
        }
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(timeout)) {
            if (mtx.try_lock()) return pdTRUE;
            std::this_thread::yield();
        }
        return mtx.try_lock() ? pdTRUE : pdFALSE;
    }

    BaseType_t giveRecursive() override {
        mtx.unlock();
        return pdTRUE;
    }
};

typedef SemaphoreBase* SemaphoreHandle_t;

inline SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t maxCount, UBaseType_t initialCount) {
    return new CountingSemaphoreMock(maxCount, initialCount);
}

inline SemaphoreHandle_t xSemaphoreCreateMutex() {
    return new MutexMock();
}

inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() {
    return new RecursiveMutexMock();
}

inline BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout) {
    if (!sem) return pdFALSE;
    return sem->take(timeout);
}

inline BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    if (!sem) return pdFALSE;
    return sem->give();
}

inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t sem, TickType_t timeout) {
    if (!sem) return pdFALSE;
    return sem->takeRecursive(timeout);
}

inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t sem) {
    if (!sem) return pdFALSE;
    return sem->giveRecursive();
}

inline void vSemaphoreDelete(SemaphoreHandle_t sem) {
    delete sem;
}
