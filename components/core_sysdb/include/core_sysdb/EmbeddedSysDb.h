#pragma once

#include "core_sysdb/SystemState.h"
#include "core_sysdb/SysDbCodec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <cstddef>
#include <atomic>

namespace HotAudioBit {
    static constexpr uint32_t ASST_SPEAKING      = (1u << 0);
    static constexpr uint32_t TURN_COMPLETE_PEND = (1u << 1);
    static constexpr uint32_t BLUETOOTH_CONN     = (1u << 2);
    static constexpr uint32_t BLUETOOTH_SETTLED  = (1u << 3);
    static constexpr uint32_t BT_CONNECTED       = BLUETOOTH_CONN | BLUETOOTH_SETTLED;
}

/**
 * @brief Authoritative singleton state database for the entire device.
 *
 *   - All mutations pass through mutate() under write lock.
 *   - Per-field change detection computes a change mask and notifies interested reactors.
 */
class EmbeddedSysDb {
public:
    static EmbeddedSysDb& getInstance();

    // ── Writer API ────────────────────────────────────────────────────────────

    /**
     * @brief Atomically mutate state and notify reactors interested in the changed fields.
     *
     * @param fn Lambda / function pointer that receives a writable SystemState&.
     *           Must return quickly — runs inside the write lock.
     */
    template <typename Fn>
    void mutate(Fn&& fn) {
        acquireWrite();
        SystemState old_state = m_state;
        fn(m_state);
        updateHotAudioFlags_locked();
        ComponentMask changed = SysDbCodec::diffState(old_state, m_state);
        if (changed > 0) {
            notifyReactors_locked(changed);
        }
        releaseWrite();
    }

    // ── Reader API ────────────────────────────────────────────────────────────

    /**
     * @brief Return a value-copy of the full state under a shared read lock.
     */
    SystemState snapshot() const;

    // ── True lockless audio hot-path readers (0 semaphores, single atomic load) ──
    uint32_t hotAudioFlags() const {
        return m_hot_audio_flags.load(std::memory_order_acquire);
    }
    bool hotAssistantSpeaking() const {
        return (hotAudioFlags() & HotAudioBit::ASST_SPEAKING) != 0;
    }
    bool hotTurnCompletePending() const {
        return (hotAudioFlags() & HotAudioBit::TURN_COMPLETE_PEND) != 0;
    }
    bool hotBluetoothConnected() const {
        return (hotAudioFlags() & HotAudioBit::BLUETOOTH_CONN) != 0;
    }
    bool hotBluetoothSettled() const {
        return (hotAudioFlags() & HotAudioBit::BLUETOOTH_SETTLED) != 0;
    }

    // Hot-path single-field getters
    bool                wifiConnected()        const;
    NetworkState        networkState()         const;
    int                 speakerVolume()        const;
    float               micGain()              const;
    bool                assistantSpeaking()    const;
    bool                micEnabled()           const;
    AssistantState      sessionState()         const;
    PipelineMode        pipelineMode()         const;
    WsState             wsState()              const;
    bool                turnCompletePending()  const;
    bool                alarmPlaying()         const;
    bool                alarmStopRequested()   const;
    MediaPlaybackState  mediaState()           const;
    bool                isMediaDucked()        const;
    bool                autoplayEnabled()      const;
    bool                cacheDownloads()       const;
    bool                bluetoothConnected()   const;
    MediaOutputTarget   mediaOutputTarget()    const;
    MediaPendingCommand mediaPendingCommand()  const;

    // ── Reactor registration ──────────────────────────────────────────────────

    /**
     * @brief Register a ReactorTask's FreeRTOS handle to receive notifications.
     */
    void registerReactor(ComponentMask interest, TaskHandle_t handle);

    static ComponentMask diffState(const SystemState& old_s, const SystemState& new_s);

private:
    EmbeddedSysDb();
    ~EmbeddedSysDb();
    EmbeddedSysDb(const EmbeddedSysDb&) = delete;
    EmbeddedSysDb& operator=(const EmbeddedSysDb&) = delete;

    // ── FreeRTOS R/W lock ─────────────────────────────────────────────────────
    static constexpr int MAX_READERS = 8;
    SemaphoreHandle_t m_read_sem;
    SemaphoreHandle_t m_write_mutex;

    void acquireRead()  const;
    void releaseRead()  const;
    void acquireWrite();
    void releaseWrite();

    // ── State ─────────────────────────────────────────────────────────────────
    SystemState   m_state;

    // ── Reactor registry ──────────────────────────────────────────────────────
    static constexpr size_t MAX_REACTORS = 12;
    struct ReactorEntry {
        ComponentMask mask   = 0;
        TaskHandle_t  handle = nullptr;
    };
    ReactorEntry m_reactors[MAX_REACTORS]{};
    size_t       m_reactor_count = 0;

    void notifyReactors_locked(ComponentMask changed);

    // ── Hot-path atomic state cache ──────────────────────────────────────────
    std::atomic<uint32_t> m_hot_audio_flags{0};
    void updateHotAudioFlags_locked();

    static constexpr const char* TAG = "SysDb";
};
