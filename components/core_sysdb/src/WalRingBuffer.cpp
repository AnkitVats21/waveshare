#include "core_sysdb/WalRingBuffer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cstring>
#include <algorithm>

static const char* TAG = "WalRing";

WalRingBuffer::WalRingBuffer(size_t capacity)
    : m_capacity(capacity)
    , m_buffer(nullptr)
    , m_head_offset(0)
    , m_tail_offset(0)
    , m_record_count(0)
    , m_current_seq(0)
    , m_oldest_seq(0)
    , m_mutex(xSemaphoreCreateMutex())
{
    // Try allocating in external PSRAM first
    m_buffer = static_cast<uint8_t*>(heap_caps_malloc(m_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!m_buffer) {
        ESP_LOGW(TAG, "PSRAM allocation failed for %zu bytes, falling back to default heap", m_capacity);
        m_buffer = static_cast<uint8_t*>(std::malloc(m_capacity));
    }

    if (!m_buffer) {
        ESP_LOGE(TAG, "Failed to allocate WAL ring buffer memory (%zu bytes)", m_capacity);
    } else {
        std::memset(m_buffer, 0, m_capacity);
        ESP_LOGI(TAG, "Allocated WAL ring buffer: %zu bytes", m_capacity);
    }
}

WalRingBuffer::~WalRingBuffer() {
    if (m_buffer) {
        heap_caps_free(m_buffer);
        m_buffer = nullptr;
    }
    if (m_mutex) {
        vSemaphoreDelete(m_mutex);
        m_mutex = nullptr;
    }
}

void WalRingBuffer::advanceTailLocked() {
    if (m_record_count == 0) return;

    if (m_tail_offset + sizeof(RecordHeader) > m_capacity) {
        m_tail_offset = 0;
        return;
    }

    const auto* hdr = reinterpret_cast<const RecordHeader*>(m_buffer + m_tail_offset);
    if (hdr->total_len == WRAP_SENTINEL) {
        m_tail_offset = 0;
        if (m_tail_offset + sizeof(RecordHeader) <= m_capacity) {
            hdr = reinterpret_cast<const RecordHeader*>(m_buffer + m_tail_offset);
        } else {
            return;
        }
    }

    uint16_t step = hdr->total_len;
    if (step < sizeof(RecordHeader) || step > m_capacity) {
        // Corrupted or invalid length, reset ring to head
        m_tail_offset = m_head_offset;
        m_record_count = 0;
        return;
    }

    m_tail_offset += step;
    m_record_count--;

    if (m_tail_offset + sizeof(RecordHeader) <= m_capacity) {
        const auto* next_hdr = reinterpret_cast<const RecordHeader*>(m_buffer + m_tail_offset);
        if (next_hdr->total_len == WRAP_SENTINEL) {
            m_tail_offset = 0;
            if (m_record_count > 0 && m_tail_offset + sizeof(RecordHeader) <= m_capacity) {
                next_hdr = reinterpret_cast<const RecordHeader*>(m_buffer + m_tail_offset);
                m_oldest_seq = next_hdr->seq;
            }
        } else if (m_record_count > 0) {
            m_oldest_seq = next_hdr->seq;
        }
    } else {
        m_tail_offset = 0;
        if (m_record_count > 0 && m_tail_offset + sizeof(RecordHeader) <= m_capacity) {
            const auto* next_hdr = reinterpret_cast<const RecordHeader*>(m_buffer + m_tail_offset);
            m_oldest_seq = next_hdr->seq;
        }
    }
}

uint32_t WalRingBuffer::push(uint8_t comp_id, uint8_t field_tag, const void* val, uint8_t val_len) {
    if (!m_buffer) return 0;

    xSemaphoreTake(m_mutex, portMAX_DELAY);

    uint16_t total_len = static_cast<uint16_t>(sizeof(RecordHeader) + val_len);
    if (total_len > m_capacity) {
        xSemaphoreGive(m_mutex);
        ESP_LOGE(TAG, "Record size %u exceeds ring capacity %zu", total_len, m_capacity);
        return 0;
    }

    // Check if we need to wrap at the end of the buffer
    if (m_head_offset + total_len > m_capacity) {
        // If tail was between head and end of buffer, advance tail past it
        while (m_record_count > 0 && m_tail_offset >= m_head_offset) {
            advanceTailLocked();
        }

        if (m_head_offset + sizeof(RecordHeader) <= m_capacity) {
            auto* wrap_hdr = reinterpret_cast<RecordHeader*>(m_buffer + m_head_offset);
            wrap_hdr->total_len = WRAP_SENTINEL;
            wrap_hdr->seq = 0;
            wrap_hdr->comp_id = 0;
            wrap_hdr->field_tag = 0;
            wrap_hdr->val_len = 0;
        }
        m_head_offset = 0;
    }

    // If writing here would overwrite the tail, advance the tail
    while (m_record_count > 0 &&
           m_head_offset <= m_tail_offset &&
           (m_head_offset + total_len) > m_tail_offset) {
        advanceTailLocked();
    }

    m_current_seq++;
    if (m_record_count == 0) {
        m_oldest_seq = m_current_seq;
        m_tail_offset = m_head_offset;
    }

    auto* hdr = reinterpret_cast<RecordHeader*>(m_buffer + m_head_offset);
    hdr->total_len = total_len;
    hdr->seq = m_current_seq;
    hdr->comp_id = comp_id;
    hdr->field_tag = field_tag;
    hdr->val_len = val_len;

    if (val_len > 0 && val != nullptr) {
        std::memcpy(m_buffer + m_head_offset + sizeof(RecordHeader), val, val_len);
    }

    m_head_offset += total_len;
    m_record_count++;

    uint32_t assigned_seq = m_current_seq;
    xSemaphoreGive(m_mutex);
    return assigned_seq;
}

uint32_t WalRingBuffer::getHeadSeq() const {
    xSemaphoreTake(m_mutex, portMAX_DELAY);
    uint32_t seq = m_current_seq;
    xSemaphoreGive(m_mutex);
    return seq;
}

uint32_t WalRingBuffer::getOldestSeq() const {
    xSemaphoreTake(m_mutex, portMAX_DELAY);
    uint32_t seq = m_oldest_seq;
    xSemaphoreGive(m_mutex);
    return seq;
}

size_t WalRingBuffer::getRecordCount() const {
    xSemaphoreTake(m_mutex, portMAX_DELAY);
    size_t count = m_record_count;
    xSemaphoreGive(m_mutex);
    return count;
}

WalQueryResult WalRingBuffer::getRecordsSince(uint32_t since_seq,
                                             std::vector<WalRecordEntry>& out_records,
                                             uint32_t max_records) const {
    if (!m_buffer) return WalQueryResult::SNAPSHOT_REQUIRED;

    xSemaphoreTake(m_mutex, portMAX_DELAY);

    if (m_record_count == 0) {
        xSemaphoreGive(m_mutex);
        return WalQueryResult::UP_TO_DATE;
    }

    if (since_seq == m_current_seq) {
        xSemaphoreGive(m_mutex);
        return WalQueryResult::UP_TO_DATE;
    }

    // If client is ahead of current server head (e.g. server rebooted and reset seq),
    // or if requested sequence has scrolled past the oldest available record in ring:
    if (since_seq > m_current_seq || (since_seq > 0 && since_seq < m_oldest_seq)) {
        xSemaphoreGive(m_mutex);
        return WalQueryResult::SNAPSHOT_REQUIRED;
    }

    out_records.clear();
    out_records.reserve(std::min<size_t>(m_record_count, max_records));

    size_t scan_offset = m_tail_offset;
    size_t inspected = 0;

    while (inspected < m_record_count && out_records.size() < max_records) {
        if (scan_offset + sizeof(RecordHeader) > m_capacity) {
            scan_offset = 0;
        }

        const auto* hdr = reinterpret_cast<const RecordHeader*>(m_buffer + scan_offset);
        if (hdr->total_len == WRAP_SENTINEL) {
            scan_offset = 0;
            if (scan_offset + sizeof(RecordHeader) > m_capacity) break;
            hdr = reinterpret_cast<const RecordHeader*>(m_buffer + scan_offset);
        }

        uint16_t step = hdr->total_len;
        if (step < sizeof(RecordHeader) || step > m_capacity) {
            break; // Corrupted entry boundary
        }

        if (hdr->seq > since_seq) {
            WalRecordEntry entry;
            entry.seq = hdr->seq;
            entry.component_id = hdr->comp_id;
            entry.field_tag = hdr->field_tag;
            if (hdr->val_len > 0) {
                const uint8_t* val_ptr = m_buffer + scan_offset + sizeof(RecordHeader);
                entry.value.assign(val_ptr, val_ptr + hdr->val_len);
            }
            out_records.push_back(std::move(entry));
        }

        scan_offset += step;
        inspected++;
    }

    xSemaphoreGive(m_mutex);
    return WalQueryResult::SUCCESS;
}

void WalRingBuffer::reset() {
    if (!m_buffer) return;
    xSemaphoreTake(m_mutex, portMAX_DELAY);
    m_head_offset = 0;
    m_tail_offset = 0;
    m_record_count = 0;
    m_current_seq = 0;
    m_oldest_seq = 0;
    std::memset(m_buffer, 0, m_capacity);
    xSemaphoreGive(m_mutex);
}
