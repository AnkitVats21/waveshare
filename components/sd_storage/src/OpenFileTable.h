#pragma once

#include <cstdint>
#include "freertos/FreeRTOS.h"

namespace sd_storage {

// Tracks which paths are open through File, keyed by a hash of the
// lower-cased path (FAT names are case-insensitive). 16 entries of 8 bytes, no
// allocation.
namespace OpenFileTable {

uint32_t key(const char* path);

// Registers an open, waiting up to `wait` for a conflicting one to close.
// A writer conflicts with any other writer and with non-following readers.
bool acquire(uint32_t key, bool writer, bool follower, TickType_t wait);
void release(uint32_t key, bool writer, bool follower);
bool isOpen(uint32_t key);

} // namespace OpenFileTable
} // namespace sd_storage
