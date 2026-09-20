#pragma once

#include "core_sysdb/WalTypes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdint>
#include <cstddef>
#include <vector>

/**
 * @brief Thread-safe PSRAM circular buffer for STAR write-ahead log records.
 *
 * Sized for ~5,000 mutations (default 64 KB). Allocates in external PSRAM on ESP32-S3
 * with transparent fallback to internal heap on host test runners.
 */
class WalRingBuffer {
public:
    static constexpr size_t DEFAULT_CAPACITY = 64 * 1024; // 64 KB

    explicit WalRingBuffer(size_t capacity = DEFAULT_CAPACITY);
    ~WalRingBuffer();

    WalRingBuffer(const WalRingBuffer&) = delete;
    WalRingBuffer& operator=(const WalRingBuffer&) = delete;

    /**
     * @brief Append a mutation record to the WAL.
     *
     * @param comp_id    Component ID
     * @param field_tag  Field index within component
     * @param val        Pointer to value payload
     * @param val_len    Length of value in bytes (<= 128)
     * @return Assigned sequence number
     */
    uint32_t push(uint8_t comp_id, uint8_t field_tag, const void* val, uint8_t val_len);

    /**
     * @brief Latest sequence number written to the log.
     */
    uint32_t getHeadSeq() const;

    /**
     * @brief Oldest sequence number still available in the retention window.
     */
    uint32_t getOldestSeq() const;

    /**
     * @brief Total number of active records currently in the ring.
     */
    size_t getRecordCount() const;

    /**
     * @brief Fetch WAL records emitted after since_seq.
     *
     * @param since_seq    Client's last received sequence number
     * @param out_records  Vector populated with missed records
     * @param max_records  Cap on records returned in this batch
     * @return SUCCESS, UP_TO_DATE, or SNAPSHOT_REQUIRED if since_seq rolled over
     */
    WalQueryResult getRecordsSince(uint32_t since_seq,
                                  std::vector<WalRecordEntry>& out_records,
                                  uint32_t max_records = 256) const;

    /**
     * @brief Reset the ring buffer and sequence counters.
     */
    void reset();

private:
    struct __attribute__((packed)) RecordHeader {
        uint16_t total_len; // sizeof(RecordHeader) + val_len
        uint32_t seq;
        uint8_t  comp_id;
        uint8_t  field_tag;
        uint8_t  val_len;
    };

    static constexpr uint16_t WRAP_SENTINEL = 0xFFFF;

    size_t   m_capacity;
    uint8_t* m_buffer;
    size_t   m_head_offset;
    size_t   m_tail_offset;
    size_t   m_record_count;
    uint32_t m_current_seq;
    uint32_t m_oldest_seq;

    mutable SemaphoreHandle_t m_mutex;

    void advanceTailLocked();
};
