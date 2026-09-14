#pragma once
#include "FreeRTOS.h"
#include "esp_heap_caps.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <condition_variable>

typedef void (*TaskFunction_t)(void*);

typedef enum {
    eNoAction = 0,
    eSetBits,
    eSetValueWithOverwrite,
    eSetValueWithoutOverwrite,
    eIncrement
} eNotifyAction;

struct TaskNotification {
    std::mutex mtx;
    std::condition_variable cv;
    uint32_t value{0};
    std::thread th;
    bool deleted{false};

    void notify(uint32_t val, eNotifyAction action) {
        std::lock_guard<std::mutex> lock(mtx);
        switch (action) {
            case eSetBits: value |= val; break;
            case eSetValueWithOverwrite: value = val; break;
            case eSetValueWithoutOverwrite: if (value == 0) value = val; break;
            case eIncrement: ++value; break;
            case eNoAction: default: break;
        }
        cv.notify_one();
    }

    void give() {
        notify(1, eIncrement);
    }

    uint32_t take(BaseType_t clear_on_exit, TickType_t timeout) {
        std::unique_lock<std::mutex> lock(mtx);
        if (value == 0) {
            if (timeout == 0) return 0;
            if (timeout == portMAX_DELAY) {
                cv.wait(lock, [this] { return value > 0; });
            } else {
                cv.wait_for(lock, std::chrono::milliseconds(timeout), [this] { return value > 0; });
            }
        }
        uint32_t res = value;
        if (clear_on_exit) value = 0;
        else if (value > 0) --value;
        return res;
    }

    BaseType_t notifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit, uint32_t* val_out, TickType_t timeout) {
        std::unique_lock<std::mutex> lock(mtx);
        value &= ~clear_on_entry;
        if (value == 0) {
            if (timeout == 0) {
                if (val_out) *val_out = 0;
                return pdFALSE;
            }
            if (timeout == portMAX_DELAY) {
                cv.wait(lock, [this] { return value > 0 || deleted; });
            } else {
                bool ok = cv.wait_for(lock, std::chrono::milliseconds(timeout), [this] { return value > 0 || deleted; });
                if (!ok) {
                    if (val_out) *val_out = 0;
                    return pdFALSE;
                }
            }
        }
        if (deleted) return pdFALSE;
        if (val_out) *val_out = value;
        value &= ~clear_on_exit;
        return pdTRUE;
    }
};

typedef TaskNotification* TaskHandle_t;

inline void vTaskDelay(TickType_t ticks) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ticks * portTICK_PERIOD_MS));
}

inline thread_local TaskHandle_t s_current_task = nullptr;

inline TaskHandle_t xTaskGetCurrentTaskHandle() {
    if (!s_current_task) {
        static thread_local TaskNotification s_default_notif;
        s_current_task = &s_default_notif;
    }
    return s_current_task;
}

inline void xTaskNotifyGive(TaskHandle_t task) {
    if (task) task->give();
}

inline BaseType_t xTaskNotify(TaskHandle_t task, uint32_t val, eNotifyAction action) {
    if (task) {
        task->notify(val, action);
        return pdPASS;
    }
    return pdFAIL;
}

inline uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t timeout) {
    return xTaskGetCurrentTaskHandle()->take(clear_on_exit, timeout);
}

inline BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit, uint32_t* val_out, TickType_t timeout) {
    return xTaskGetCurrentTaskHandle()->notifyWait(clear_on_entry, clear_on_exit, val_out, timeout);
}

inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack_depth, void* arg, UBaseType_t prio, TaskHandle_t* out_handle, BaseType_t core_id) {
    (void)name; (void)stack_depth; (void)prio; (void)core_id;
    TaskNotification* task = new TaskNotification();
    if (out_handle) *out_handle = task;
    task->th = std::thread([fn, arg, task]() {
        s_current_task = task;
        fn(arg);
    });
    task->th.detach();
    return pdPASS;
}

inline BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char* name, uint32_t stack_depth, void* arg, UBaseType_t prio, TaskHandle_t* out_handle, BaseType_t core_id, uint32_t caps) {
    (void)caps;
    return xTaskCreatePinnedToCore(fn, name, stack_depth, arg, prio, out_handle, core_id);
}

inline void vTaskDelete(TaskHandle_t task) {
    if (!task) task = xTaskGetCurrentTaskHandle();
    if (task) {
        std::lock_guard<std::mutex> lock(task->mtx);
        task->deleted = true;
        task->cv.notify_all();
    }
}
