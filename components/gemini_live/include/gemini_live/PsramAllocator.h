#pragma once

#include <ArduinoJson.h>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>

struct PsramAllocator : ArduinoJson::Allocator {
    void* allocate(size_t size) override {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    void deallocate(void* ptr) override {
        heap_caps_free(ptr);
    }
    void* reallocate(void* ptr, size_t new_size) override {
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
};
#else
#include <cstdlib>

struct PsramAllocator : ArduinoJson::Allocator {
    void* allocate(size_t size) override {
        return malloc(size);
    }
    void deallocate(void* ptr) override {
        free(ptr);
    }
    void* reallocate(void* ptr, size_t new_size) override {
        return realloc(ptr, new_size);
    }
};
#endif
