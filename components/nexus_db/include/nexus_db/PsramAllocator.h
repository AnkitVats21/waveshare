#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace nexus_db {

// Allocator for the index and cached documents. Small allocations would
// otherwise land in internal RAM (SPIRAM_MALLOC_ALWAYSINTERNAL), and a
// database holds thousands of them.
template <typename T>
struct PsramAllocator {
    using value_type = T;

    PsramAllocator() noexcept = default;
    template <typename U>
    PsramAllocator(const PsramAllocator<U>&) noexcept {}

    T* allocate(size_t n) {
#ifdef ESP_PLATFORM
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) p = malloc(n * sizeof(T));
#else
        void* p = malloc(n * sizeof(T));
#endif
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t) noexcept { free(p); }

    template <typename U>
    bool operator==(const PsramAllocator<U>&) const noexcept { return true; }
    template <typename U>
    bool operator!=(const PsramAllocator<U>&) const noexcept { return false; }
};

using PsramString = std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;

} // namespace nexus_db
