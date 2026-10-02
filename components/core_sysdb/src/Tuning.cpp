#include "core_sysdb/Tuning.h"
#include "freertos/FreeRTOS.h"
#include <cstring>

namespace {

Tuning::Flag s_flags[Tuning::kMaxFlags];
size_t s_count = 0;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

// Caller holds s_lock.
Tuning::Flag* find(const char* key) {
    for (size_t i = 0; i < s_count; ++i) {
        if (std::strncmp(s_flags[i].key, key, Tuning::kKeyLen - 1) == 0) return &s_flags[i];
    }
    return nullptr;
}

// Caller holds s_lock. Null when the table is full.
Tuning::Flag* add(const char* key, int32_t value, bool from_file) {
    if (s_count >= Tuning::kMaxFlags) return nullptr;
    Tuning::Flag& f = s_flags[s_count++];
    std::strncpy(f.key, key, Tuning::kKeyLen - 1);
    f.key[Tuning::kKeyLen - 1] = '\0';
    f.value = value;
    f.from_file = from_file;
    f.read = false;
    return &f;
}

}  // namespace

namespace Tuning {

void set(const char* key, int32_t value) {
    portENTER_CRITICAL(&s_lock);
    if (Flag* f = find(key)) {
        f->value = value;
        f->from_file = true;
    } else {
        add(key, value, true);
    }
    portEXIT_CRITICAL(&s_lock);
}

int32_t get(const char* key, int32_t def) {
    portENTER_CRITICAL(&s_lock);
    Flag* f = find(key);
    if (!f) f = add(key, def, false);
    int32_t value = def;
    if (f) {
        f->read = true;
        if (f->from_file) value = f->value;
    }
    portEXIT_CRITICAL(&s_lock);
    return value;
}

size_t list(Flag* out, size_t max) {
    portENTER_CRITICAL(&s_lock);
    const size_t n = s_count < max ? s_count : max;
    std::memcpy(out, s_flags, n * sizeof(Flag));
    portEXIT_CRITICAL(&s_lock);
    return n;
}

}  // namespace Tuning
