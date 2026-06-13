#pragma once

#include "common/sysdb/EmbeddedSysDb.h"
#include "common/sysdb/SystemState.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * @brief Base class for all state-reactive services.
 *
 * ## What it does
 *   1. Spawns a FreeRTOS task (in its constructor) pinned to a given core.
 *   2. Registers with EmbeddedSysDb so it receives xTaskNotifyGive() whenever
 *      any watched component is mutated.
 *   3. The internal task loop calls ulTaskNotifyTake() to block, then invokes
 *      onStateChanged() with a fresh snapshot and the changed component mask.
 *   4. After onStateChanged() returns, run() is called to do any heavy work
 *      (hardware I/O, clock switches, etc.).
 *
 * ## Contract for subclasses
 *   - **onStateChanged()** must return quickly (< 5 µs ideally).
 *     Set atomic flags, issue xTaskNotifyGive to worker tasks, update local
 *     state. Do NOT call blocking I2S / HAL APIs here.
 *   - **run()** does the heavy lifting. Call ulTaskNotifyTake() at the top
 *     of its loop to wait for the next notification.
 *
 * ## Example
 * ```cpp
 * class AudioService : public ReactorTask {
 * public:
 *     AudioService(AudioHal& hal)
 *         : ReactorTask({ "audio_svc", 6144, 10, 1, COMP::AUDIO | COMP::PIPELINE })
 *         , m_hal(hal) {}
 * protected:
 *     void onStateChanged(ComponentMask changed, const SystemState& snap) override {
 *         if (changed & COMP::AUDIO) {
 *             m_pending_volume = snap.audio.speaker_volume;
 *             xTaskNotifyGive(m_task_handle); // wake run() loop
 *         }
 *     }
 *     void run() override {
 *         while (m_running) {
 *             ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
 *             m_hal.setPlayVolume(m_pending_volume);
 *         }
 *     }
 * };
 * ```
 */
class ReactorTask {
public:
    struct Config {
        const char*   name;        ///< FreeRTOS task name
        uint32_t      stack_size;  ///< Stack in bytes
        UBaseType_t   priority;    ///< FreeRTOS priority (use ThreadConfig::Priority)
        BaseType_t    core_id;     ///< Core affinity (use ThreadConfig::CORE_*)
        ComponentMask interest;    ///< OR'd COMP:: bits this reactor watches
    };

    /**
     * @brief Construct the reactor task.
     * Task creation is deferred to start().
     */
    explicit ReactorTask(const Config& cfg);

    virtual ~ReactorTask();

    /**
     * @brief Spawn the FreeRTOS task and register with EmbeddedSysDb.
     */
    bool start();

    TaskHandle_t getHandle() const { return m_task_handle; }
    bool         isRunning() const { return m_running; }

    /**
     * @brief Called by EmbeddedSysDb (via xTaskNotifyGive) when watched components change.
     *
     * Executes in the reactor's OWN task context (not the writer's context).
     * The task receives the notification, takes a snapshot, then calls this.
     *
     * @param changed  OR'd COMP:: mask of what was mutated in this notification.
     * @param snap     Value-copy of SystemState at the time of the snapshot.
     */
    virtual void onStateChanged(ComponentMask changed, const SystemState& snap) = 0;

protected:
    /**
     * @brief Background worker loop — implement all heavy / blocking work here.
     *
     * The default implementation calls onStateChanged() after each
     * ulTaskNotifyTake(). Override if you need a different loop structure
     * (e.g., a timed poll loop that also reacts to state changes).
     *
     * If overriding, you MUST call ulTaskNotifyTake(pdTRUE, portMAX_DELAY)
     * (or similar) to yield the CPU when idle.
     */
    virtual void run();

    Config        m_cfg;
    TaskHandle_t  m_task_handle = nullptr;
    volatile bool m_running     = false;

    /**
     * @brief Component mask of what changed in the most recent notification.
     * Set by the task loop before calling onStateChanged(). Read-only inside
     * onStateChanged().
     */
    ComponentMask m_last_changed = 0;

private:
    static void taskEntry(void* param);
};
