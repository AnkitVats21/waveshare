#include "services/BufferManager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

BufferManager &BufferManager::getInstance() {
    static BufferManager instance;
    return instance;
}

// ----------------------------------------------------------------------------
// registerDescriptor — called from DEFINE_BUFFER constructor before app_main
// ----------------------------------------------------------------------------
void BufferManager::registerDescriptor(BufferId &out_id, const char *name,
                                        size_t bytes) {
    if (m_count >= MAX_BUFFERS) {
        ESP_LOGE(TAG, "MAX_BUFFERS (%d) exceeded — increase limit", MAX_BUFFERS);
        out_id = INVALID;
        return;
    }
    uint8_t idx         = m_count++;
    m_entries[idx].name       = name;
    m_entries[idx].size_bytes = bytes;
    m_entries[idx].registered = true;
    out_id = idx;
    ESP_LOGD(TAG, "Registered buffer [%d] '%s' (%u bytes)", idx, name,
             (unsigned)bytes);
}

// ----------------------------------------------------------------------------
// initAll — allocates every registered buffer from PSRAM
// ----------------------------------------------------------------------------
bool BufferManager::initAll() {
    bool ok = true;
    for (uint8_t i = 0; i < m_count; ++i) {
        Entry &e = m_entries[i];
        if (!e.registered || e.allocated)
            continue;

        e.rbuf = static_cast<RingbufHandle_t>(
            xRingbufferCreateWithCaps(e.size_bytes, RINGBUF_TYPE_BYTEBUF,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!e.rbuf) {
            ESP_LOGE(TAG, "Failed to allocate PSRAM ring buffer '%s' (%u B)",
                     e.name, (unsigned)e.size_bytes);
            ok = false;
        } else {
            e.allocated = true;
            ESP_LOGI(TAG, "Allocated '%s' (%u KB in PSRAM)",
                     e.name, (unsigned)(e.size_bytes / 1024));
        }
    }
    return ok;
}

// ----------------------------------------------------------------------------
// handle — return the raw FreeRTOS handle
// ----------------------------------------------------------------------------
RingbufHandle_t BufferManager::handle(BufferId id) const {
    if (id == INVALID || id >= MAX_BUFFERS)
        return nullptr;
    return m_entries[id].rbuf;
}

// ----------------------------------------------------------------------------
// send — write data; increment drop counter on failure
// ----------------------------------------------------------------------------
bool BufferManager::send(BufferId id, const void *data, size_t bytes,
                          TickType_t timeout) {
    RingbufHandle_t rb = handle(id);
    if (!rb) return false;
    bool ok = (xRingbufferSend(rb, data, bytes, timeout) == pdTRUE);
    if (!ok && id < MAX_BUFFERS) {
        m_entries[id].drops++;
    }
    return ok;
}

// ----------------------------------------------------------------------------
// receive — read one chunk; max_bytes=0 means read a full item
// ----------------------------------------------------------------------------
void *BufferManager::receive(BufferId id, size_t *bytes_out,
                              TickType_t timeout, size_t max_bytes) {
    RingbufHandle_t rb = handle(id);
    if (!rb) { if (bytes_out) *bytes_out = 0; return nullptr; }
    if (max_bytes > 0) {
        return xRingbufferReceiveUpTo(rb, bytes_out, timeout, max_bytes);
    } else {
        return xRingbufferReceive(rb, bytes_out, timeout);
    }
}

// ----------------------------------------------------------------------------
// returnItem — null-safe item return
// ----------------------------------------------------------------------------
void BufferManager::returnItem(BufferId id, void *item) {
    if (!item) return;
    RingbufHandle_t rb = handle(id);
    if (rb) vRingbufferReturnItem(rb, item);
}

// ----------------------------------------------------------------------------
// flush — non-blocking drain (replaces while-loop pattern at call sites)
// ----------------------------------------------------------------------------
void BufferManager::flush(BufferId id) {
    RingbufHandle_t rb = handle(id);
    if (!rb) return;
    size_t sz = 0;
    void  *item;
    while ((item = xRingbufferReceive(rb, &sz, 0)) != nullptr) {
        vRingbufferReturnItem(rb, item);
    }
}

// ----------------------------------------------------------------------------
// destroy — return PSRAM to heap and invalidate slot
// ----------------------------------------------------------------------------
void BufferManager::destroy(BufferId id) {
    if (id == INVALID || id >= MAX_BUFFERS) return;
    Entry &e = m_entries[id];
    if (e.rbuf) {
        vRingbufferDelete(e.rbuf);
        e.rbuf      = nullptr;
        e.allocated = false;
    }
}

// ----------------------------------------------------------------------------
// dumpStats — log all buffer states (handy for heap audits)
// ----------------------------------------------------------------------------
void BufferManager::dumpStats() const {
    ESP_LOGI(TAG, "=== BufferManager stats (%d registered) ===", m_count);
    for (uint8_t i = 0; i < m_count; ++i) {
        const Entry &e = m_entries[i];
        if (!e.registered) continue;
        if (!e.allocated) {
            ESP_LOGI(TAG, "  [%d] %-16s  NOT ALLOCATED", i, e.name);
            continue;
        }
        size_t used = getUsedBytes(i);
        ESP_LOGI(TAG, "  [%d] %-16s  %5u KB total  %5u B used  %4u drops",
                 i, e.name,
                 (unsigned)(e.size_bytes / 1024),
                 (unsigned)used,
                 (unsigned)e.drops);
    }
}
