#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

namespace sd_storage {

struct DirEntry {
    const char* name;
    bool is_dir;
    size_t size;   // only filled when listing with_stat
    time_t mtime;  // only filled when listing with_stat
};

struct PathInfo {
    bool is_dir = false;
    size_t size = 0;
    time_t mtime = 0;
};

// Whole-file and directory operations on the SD card, built on File.
namespace Fs {

// Whole file into a string; empty if missing, empty, or over max_bytes.
std::string readText(const char* path, size_t max_bytes = 256 * 1024);
// Up to `len` bytes from the start of the file; returns bytes read.
size_t readInto(const char* path, void* dst, size_t len);

// Replace a file so that a power cut leaves either the old or the new
// contents: write <path>.tmp, fsync, then swap it in. Reads finish an
// interrupted swap.
bool writeAtomic(const char* path, const void* data, size_t len);
bool writeAtomic(const char* path, const char* text);
bool append(const char* path, const void* data, size_t len);

bool exists(const char* path);
bool isFile(const char* path);
bool isDir(const char* path);
bool stat(const char* path, PathInfo& out);

// Files only. Fails while the file is open.
bool remove(const char* path);
// File or empty directory. Fails while the file is open.
bool removePath(const char* path);
// Fails while either path is open, or if new_path exists.
bool rename(const char* old_path, const char* new_path);
// Creates every missing directory along the path.
bool mkdirs(const char* dir_path);

// Calls cb for each entry except "." and "..". `ext` (e.g. ".ogg", case
// insensitive) filters files and skips directories; nullptr lists everything.
// Return false from cb to stop early.
typedef bool (*ListCallback)(const DirEntry& entry, void* ctx);
bool list(const char* dir_path, const char* ext, bool with_stat, ListCallback cb, void* ctx);

bool info(uint64_t& total_bytes, uint64_t& free_bytes);

} // namespace Fs
} // namespace sd_storage
