#pragma once

#include "core_sysdb/SystemState.h"
#include "core_sysdb/WalTypes.h"
#include "core_sysdb/WalRingBuffer.h"
#include "core_sysdb/SysDbCodec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <cstddef>
#include <atomic>
#include <vector>

namespace HotAudioBit {
    static constexpr uint32_t ASST_SPEAKING      = (1u << 0);
    static constexpr uint32_t TURN_COMPLETE_PEND = (1u << 1);
    static constexpr uint32_t COMPANION_CONN     = (1u << 2);
    static constexpr uint32_t COMPANION_SETTLED  = (1u << 3);
    static constexpr uint32_t BT_CONNECTED       = COMPANION_CONN | COMPANION_SETTLED;
}

/**
 * @brief Authoritative singleton state database for the entire device.
 *
 * Implements STAR (Single-writer, Tagged-field, Asynchronous Replication):
 *   - The device holds the authoritative state.
 *   - All mutations pass through mutate() under write lock.
 *   - Automatic per-field change detection emits records into a PSRAM WAL ring buffer.
 *   - External writes are validated at a single choke point against compile-time FieldAccess.
 */
class EmbeddedSysDb {
public:
    static EmbeddedSysDb& getInstance();

    // ── Writer API ────────────────────────────────────────────────────────────

    /**
     * @brief Atomically mutate state, emit WAL records for changed fields, and notify reactors.
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
        ComponentMask changed = diffAndEmitWal_locked(old_state, m_state);
        if (changed > 0) {
            notifyReactors_locked(changed);
        }
        releaseWrite();
    }

    /**
     * @brief Validate and execute a remote write request from WebSocket/REST through the write-gate.
     *
     * Rejects ReadOnly fields structurally without taking the write lock.
     */
    WriteResult processRemoteWrite(ComponentId comp, uint8_t field_tag, const uint8_t* val, uint8_t val_len);

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
    bool hotCompanionConnected() const {
        return (hotAudioFlags() & HotAudioBit::COMPANION_CONN) != 0;
    }
    bool hotCompanionSettled() const {
        return (hotAudioFlags() & HotAudioBit::COMPANION_SETTLED) != 0;
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

    // Backward-compatibility aliases
    bool btCompanionConnected()   const { return bluetoothConnected(); }
    bool btCompanionLinkSettled() const { return bluetoothConnected(); }

    // ── STAR Replication API ──────────────────────────────────────────────────

    /**
     * @brief Access the WAL ring buffer directly.
     */
    WalRingBuffer& getWal() { return m_wal; }
    const WalRingBuffer& getWal() const { return m_wal; }

    uint32_t walHeadSeq() const { return m_wal.getHeadSeq(); }

    /**
     * @brief Catch-up query for STAR clients.
     */
    WalQueryResult getWalRecordsSince(uint32_t since_seq,
                                      std::vector<WalRecordEntry>& out_records,
                                      uint32_t max_records = 256) const {
        return m_wal.getRecordsSince(since_seq, out_records, max_records);
    }

    /**
     * @brief Export full state as serialized WAL records with current head_seq.
     */
    uint32_t exportSnapshot(std::vector<WalRecordEntry>& out_snapshot) const;

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
    WalRingBuffer m_wal;

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

    ComponentMask diffAndEmitWal_locked(const SystemState& old_s, const SystemState& new_s);

    static constexpr const char* TAG = "SysDb";
};
