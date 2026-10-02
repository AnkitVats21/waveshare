#pragma once
#include <cstddef>
#include <cstdint>

// Named integer flags for performance experiments, set at boot from
// /sdcard/tuning.json (main/services/storage/TuningFile.h), so a setting can
// be A/B tested with a reboot instead of a reflash.
//
// Code reads a flag with its built-in default: Tuning::get("ww_detect_core", 0).
// Every key read is recorded, so /api/system/tuning lists the flags this
// firmware knows, their values, and whether the file set them. Keys longer
// than kKeyLen - 1 are truncated; at most kMaxFlags distinct keys.
namespace Tuning {

constexpr size_t kMaxFlags = 24;
constexpr size_t kKeyLen = 32;

struct Flag {
    char key[kKeyLen];
    int32_t value;
    bool from_file;  // false: the built-in default
    bool read;       // some code asked for it (a file key nothing reads is a typo)
};

// Stores a value from the file. Call before the code that reads the flag.
void set(const char* key, int32_t value);

// The value set from the file, else `def` (and records the key and default).
int32_t get(const char* key, int32_t def);

// Snapshot of the table for the API; returns the number of entries copied.
size_t list(Flag* out, size_t max);

}  // namespace Tuning
