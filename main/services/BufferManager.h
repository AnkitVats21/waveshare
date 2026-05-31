#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include <cstdint>

/**
 * @file BufferManager.h
 * @brief Centralized PSRAM ring-buffer registry.
 *
 * Each subsystem that needs a ring buffer declares its own slot
 * using the DECLARE_BUFFER / DEFINE_BUFFER macro pair:
 *
 *   // In the owning subsystem header (.h):
 *   DECLARE_BUFFER(MIC_TX_BUF, "mic_tx", 128 * 1024)
 *
 *   // In the owning subsystem source (.cpp):
 *   DEFINE_BUFFER(MIC_TX_BUF, "mic_tx", 128 * 1024)
 *
 * Then in SystemContext::init() call:
 *   BufferManager::getInstance().initAll();
 *
 * Consumers retrieve the handle with:
 *   RingbufHandle_t rb = BufferManager::getInstance().handle(Buffers::MIC_TX_BUF);
 */

class BufferManager {
public:
    using BufferId = uint8_t;
    static constexpr BufferId INVALID = 0xFF;
    static constexpr uint8_t  MAX_BUFFERS = 16;

    static BufferManager &getInstance();

    // ------------------------------------------------------------------ //
    // Called by DEFINE_BUFFER at static-init time (before app_main).     //
    // ------------------------------------------------------------------ //
    void registerDescriptor(BufferId &out_id, const char *name, size_t bytes);

    // ------------------------------------------------------------------ //
    // Call once, early in SystemContext::init(), before any task starts.  //
    // ------------------------------------------------------------------ //
    bool initAll();

    // ------------------------------------------------------------------ //
    // Runtime accessors                                                   //
    // ------------------------------------------------------------------ //

    /** Retrieve the raw FreeRTOS handle. Returns nullptr if id is INVALID. */
    RingbufHandle_t handle(BufferId id) const;

    /**
     * @brief Write data into a ring buffer.
     * @param timeout FreeRTOS ticks to wait if full (default: 0 = drop-on-full).
     * @return true if accepted, false if dropped (full or invalid id).
     *         Drop count is tracked and shown in dumpStats().
     */
    bool send(BufferId id, const void *data, size_t bytes,
              TickType_t timeout = 0);

    /**
     * @brief Receive up to max_bytes from a ring buffer.
     *        Caller MUST call returnItem() on the returned pointer.
     * @param max_bytes 0 = no limit (returns one full item).
     * @return Non-null on success, nullptr on timeout or invalid id.
     */
    void *receive(BufferId id, size_t *bytes_out,
                  TickType_t timeout, size_t max_bytes = 0);

    /**
     * @brief Return an item obtained via receive(). Safe to call with nullptr.
     */
    void returnItem(BufferId id, void *item);

    /** Non-blocking drain — replaces the "while(receive(0)) return" loop. */
    void flush(BufferId id);

    /** Release a single buffer and set id to INVALID. */
    void destroy(BufferId id);

    /** Log name / size / free bytes / drop count for all live buffers. */
    void dumpStats() const;

private:
    BufferManager() = default;

    struct Entry {
        RingbufHandle_t rbuf       = nullptr;
        const char     *name       = nullptr;
        size_t          size_bytes = 0;
        uint32_t        drops      = 0;     ///< send() calls that were dropped
        bool            registered = false; ///< descriptor added by macro
        bool            allocated  = false; ///< initAll() ran successfully
    };

    Entry   m_entries[MAX_BUFFERS] = {};
    uint8_t m_count = 0;

    static constexpr const char *TAG = "BufMgr";
};

// ============================================================================
// Declaration / Definition macros
// ============================================================================

/**
 * @brief Declare a named buffer slot.
 *        Place this in the .h of the subsystem that owns the buffer.
 *
 * Generates:
 *  - An extern BufferId symbol inside namespace Buffers.
 *  - A small registration helper struct (constructor runs at static-init).
 *
 * Example:
 *   DECLARE_BUFFER(MIC_TX_BUF, "mic_tx", 128 * 1024)
 */
#define DECLARE_BUFFER(id_name, buf_name_str, buf_size_bytes)          \
    namespace Buffers {                                                \
        extern BufferManager::BufferId id_name;                        \
        struct _Reg_##id_name {                                        \
            _Reg_##id_name();                                          \
        };                                                             \
        extern _Reg_##id_name _reg_instance_##id_name;                 \
    }

/**
 * @brief Define (allocate storage for) a declared buffer.
 *        Place this in the .cpp of the subsystem that owns the buffer.
 *
 * The constructor of _Reg_Xxx runs before app_main and calls
 * BufferManager::registerDescriptor() to record the name + size.
 * BufferManager::initAll() later does the actual PSRAM allocation.
 *
 * Example:
 *   DEFINE_BUFFER(MIC_TX_BUF, "mic_tx", 128 * 1024)
 */
#define DEFINE_BUFFER(id_name, buf_name_str, buf_size_bytes)           \
    namespace Buffers {                                                \
        BufferManager::BufferId id_name = BufferManager::INVALID;      \
        _Reg_##id_name _reg_instance_##id_name;                        \
        _Reg_##id_name::_Reg_##id_name() {                             \
            BufferManager::getInstance().registerDescriptor(           \
                id_name, buf_name_str, buf_size_bytes);                \
        }                                                              \
    }
