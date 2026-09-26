#include "OpenFileTable.h"

#include <cctype>
#include "freertos/task.h"
#include "esp_log.h"

namespace sd_storage {
namespace OpenFileTable {

namespace {
const char* TAG = "sd_storage";

struct Entry {
    uint32_t key;
    uint8_t writers;
    uint8_t readers;   // readers that must not overlap a writer
    uint8_t followers; // readers that may
};

constexpr int kEntries = 16;
Entry s_entries[kEntries] = {};
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

Entry* find(uint32_t k) {
    for (Entry& e : s_entries) {
        if (e.key == k && (e.writers | e.readers | e.followers)) return &e;
    }
    return nullptr;
}

Entry* findFree() {
    for (Entry& e : s_entries) {
        if (!(e.writers | e.readers | e.followers)) return &e;
    }
    return nullptr;
}

// Called with s_lock held. Returns true if the open was registered.
bool tryAcquire(uint32_t k, bool writer, bool follower) {
    Entry* e = find(k);
    if (e) {
        if (writer && (e->writers || e->readers)) return false;
        if (!writer && !follower && e->writers) return false;
    } else {
        e = findFree();
        if (!e) return false;
        *e = {k, 0, 0, 0};
    }
    if (writer) e->writers++;
    else if (follower) e->followers++;
    else e->readers++;
    return true;
}
} // namespace

uint32_t key(const char* path) {
    // FNV-1a; 0 is reserved for "unused".
    uint32_t h = 2166136261u;
    for (const char* p = path; *p; ++p) {
        h ^= static_cast<uint8_t>(tolower(static_cast<unsigned char>(*p)));
        h *= 16777619u;
    }
    return h ? h : 1;
}

bool acquire(uint32_t k, bool writer, bool follower, TickType_t wait) {
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        taskENTER_CRITICAL(&s_lock);
        bool ok = tryAcquire(k, writer, follower);
        taskEXIT_CRITICAL(&s_lock);
        if (ok) return true;
        if (xTaskGetTickCount() - start >= wait) {
            ESP_LOGW(TAG, "open conflict: file is busy (%s)", writer ? "write" : "read");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void release(uint32_t k, bool writer, bool follower) {
    taskENTER_CRITICAL(&s_lock);
    Entry* e = find(k);
    if (e) {
        if (writer && e->writers) e->writers--;
        else if (follower && e->followers) e->followers--;
        else if (!writer && !follower && e->readers) e->readers--;
    }
    taskEXIT_CRITICAL(&s_lock);
}

bool isOpen(uint32_t k) {
    taskENTER_CRITICAL(&s_lock);
    bool open = find(k) != nullptr;
    taskEXIT_CRITICAL(&s_lock);
    return open;
}

} // namespace OpenFileTable
} // namespace sd_storage
