#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/**
 * @brief Abstract Base Class for FreeRTOS Tasks
 * Standardizes task creation, core affinity, and lifecycle.
 */
class TaskBase {
public:
    struct Config {
        const char* name;
        uint32_t stack_size;
        UBaseType_t priority;
        BaseType_t core_id; // tskNO_AFFINITY, 0, or 1
    };

    TaskBase(const Config& config) : m_config(config) {}
    virtual ~TaskBase() { stop(); }

    /**
     * @brief Create and start the FreeRTOS task.
     * @return true if task was created successfully.
     */
    bool start() {
        if (m_task_handle != nullptr) return true;

        BaseType_t ret = xTaskCreatePinnedToCore(
            taskEntry,
            m_config.name,
            m_config.stack_size,
            this,
            m_config.priority,
            &m_task_handle,
            m_config.core_id
        );

        if (ret != pdPASS) {
            ESP_LOGE("TaskBase", "Failed to create task: %s", m_config.name);
            return false;
        }
        
        m_running = true;
        return true;
    }

    /**
     * @brief Stop and delete the task.
     */
    void stop() {
        if (m_task_handle != nullptr) {
            m_running = false;
            vTaskDelete(m_task_handle);
            m_task_handle = nullptr;
        }
    }

    TaskHandle_t getHandle() const { return m_task_handle; }

protected:
    /**
     * @brief The actual logic loop implemented by the derived class.
     */
    virtual void run() = 0;

    /**
     * @brief Static entry point required by FreeRTOS.
     */
    static void taskEntry(void* param) {
        TaskBase* self = static_cast<TaskBase*>(param);
        self->run();
        
        // Safety: If run() ever returns, clean up the handle
        self->m_task_handle = nullptr;
        self->m_running = false;
        vTaskDelete(nullptr);
    }

    Config m_config;
    TaskHandle_t m_task_handle = nullptr;
    volatile bool m_running = false;
};
