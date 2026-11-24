#pragma once

// micro-opus gives every task that runs Opus its own scratch buffer (its
// "pseudostack", CONFIG_OPUS_PSEUDOSTACK_SIZE) on the task's first
// opus_encode / opus_decode, and doesn't check the allocation: when PSRAM is
// short it gets NULL and the codec writes through it (StoreProhibited at 0,
// docs/known-issues.md). Call opusScratchReady() right before each
// opus_encode / opus_decode: it allocates the buffer itself and returns
// false instead when it can't, so the caller fails that one frame and can try
// again later. Once a task has its buffer this is one thread-local read.

#include "sdkconfig.h"

#if CONFIG_OPUS_THREADSAFE_PSEUDOSTACK
#include "esp_heap_caps.h"
#include "esp_log.h"

// Defined in micro-opus patches/thread_local_stack.c.
extern "C" {
extern thread_local char* global_stack;
char* _opus_alloc_and_register_pseudostack(void);
}
#endif

namespace Media {

inline bool opusScratchReady() {
#if CONFIG_OPUS_THREADSAFE_PSEUDOSTACK
    if (global_stack) return true;
    // Same allocation (and cleanup registration) micro-opus's ALLOC_STACK
    // makes; on success ALLOC_STACK finds global_stack set and keeps it.
    char* buf = _opus_alloc_and_register_pseudostack();
    static thread_local bool s_logged = false;
    if (!buf) {
        if (!s_logged) {
            s_logged = true;
            ESP_LOGE("OpusScratch", "No %d-byte Opus scratch for this task (PSRAM largest block %u): skipping frames",
                     CONFIG_OPUS_PSEUDOSTACK_SIZE,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        }
        return false;
    }
    if (s_logged) {
        s_logged = false;
        ESP_LOGI("OpusScratch", "Opus scratch allocated after an earlier failure");
    }
    global_stack = buf;
#endif
    return true;
}

} // namespace Media
