#pragma once

#include "common/sysdb/SystemState.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <cstddef>

/**
 * @brief Thread-safe singleton state database for the entire application.
 *
 * Replaces EventBus + GlobalSystemSettings + scattered service member vars.
 *
 * ## Concurrency model
 *   - **Multiple concurrent readers** via a counting semaphore
 *     (up to MAX_READERS simultaneous snapshots).
 *   - **Single exclusive writer** via a mutex; also holds all reader slots.
 *   - Writer calls notifyReactors_locked() inside the write lock, issuing
 *     xTaskNotifyGive() to every ReactorTask that registered interest in
 *     the mutated component(s).
 *
 * ## Usage — writers
 * ```cpp
 * EmbeddedSysDb::getInstance().mutate(COMP::AUDIO, [](SystemState& s) {
 *     s.audio.speaker_volume = 70;
 * });
 * ```
 *
 * ## Usage — readers (snapshot)
 * ```cpp
 * auto snap = EmbeddedSysDb::getInstance().snapshot();
 * int vol = snap.audio.speaker_volume;
 * ```
 *
 * ## Usage — hot-path single-field getters
 * ```cpp
 * bool speaking = EmbeddedSysDb::getInstance().assistantSpeaking();
 * ```
 *
 * ## Usage — ReactorTask registration (called from ReactorTask constructor)
 * ```cpp
 * EmbeddedSysDb::getInstance().registerReactor(COMP::AUDIO | COMP::ASSISTANT, handle);
 * ```
 */
class EmbeddedSysDb {
public:
    static EmbeddedSysDb& getInstance();

    // ── Writer API ────────────────────────────────────────────────────────────

    /**
     * @brief Atomically mutate state and notify interested reactors.
     *
     * @param comp  OR'd COMP:: bitmask indicating which components are touched.
     * @param fn    Lambda / function pointer that receives a writable SystemState&.
     *              Must return quickly — runs inside the write lock.
     */
    template <typename Fn>
    void mutate(ComponentMask comp, Fn&& fn) {
        acquireWrite();
        fn(m_state);
        notifyReactors_locked(comp);
        releaseWrite();
    }

    // ── Reader API ────────────────────────────────────────────────────────────

    /**
     * @brief Return a value-copy of the full state under a shared read lock.
     *
     * Safe to call from any task or ISR-adjacent context.
     * For hot paths prefer the typed single-field getters below.
     */
    SystemState snapshot() const;

    // Hot-path single-field getters (shorter lock window than full snapshot)
    bool           wifiConnected()    const;
    int            speakerVolume()    const;
    float          micGain()          const;
    bool           assistantSpeaking() const;
    bool           micEnabled()       const;
    AssistantState sessionState()     const;
    PipelineMode   pipelineMode()     const;

    // ── Reactor registration ──────────────────────────────────────────────────

    /**
     * @brief Register a ReactorTask's FreeRTOS handle to receive notifications.
     *
     * Call from the ReactorTask constructor at boot time only.
     * @param interest  OR'd COMP:: bitmask of components this reactor cares about.
     * @param handle    FreeRTOS task handle to notify via xTaskNotifyGive().
     */
    void registerReactor(ComponentMask interest, TaskHandle_t handle);

private:
    EmbeddedSysDb();
    ~EmbeddedSysDb();
    EmbeddedSysDb(const EmbeddedSysDb&) = delete;
    EmbeddedSysDb& operator=(const EmbeddedSysDb&) = delete;

    // ── FreeRTOS R/W lock ─────────────────────────────────────────────────────
    // Pattern: counting semaphore gives MAX_READERS "read tokens".
    // Reader: takes 1 token  / releases 1 token.
    // Writer: takes ALL tokens (exclusive) / releases all.
    static constexpr int MAX_READERS = 8;
    SemaphoreHandle_t m_read_sem;   ///< Counting semaphore (initial count = MAX_READERS)
    SemaphoreHandle_t m_write_mutex;///< Protects the "take all slots" writer sequence

    void acquireRead()  const;
    void releaseRead()  const;
    void acquireWrite();
    void releaseWrite();

    // ── State ─────────────────────────────────────────────────────────────────
    SystemState m_state;

    // ── Reactor registry ──────────────────────────────────────────────────────
    static constexpr size_t MAX_REACTORS = 12;
    struct ReactorEntry {
        ComponentMask mask   = 0;
        TaskHandle_t  handle = nullptr;
    };
    ReactorEntry m_reactors[MAX_REACTORS]{};
    size_t       m_reactor_count = 0;

    /**
     * @brief Called inside the write lock — issues xTaskNotifyGive() to all
     *        registered reactors whose interest mask overlaps @p changed.
     */
    void notifyReactors_locked(ComponentMask changed);

    static constexpr const char* TAG = "SysDb";
};
