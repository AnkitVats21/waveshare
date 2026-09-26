#pragma once

#include <cstddef>
#include <string>

namespace sd_storage {

// Rules for paths that come from outside the firmware (HTTP file API, Gemini
// file tools).
namespace PathPolicy {

// Normalises separators and duplicate slashes, rejects "..", and roots the
// result under base_mount. Returns false if the path is not allowed.
bool sanitize(const char* in_path, std::string& out_path, const char* base_mount = "/sdcard");

// Files that must never be read, written, renamed or deleted from outside
// (they hold credentials). The list is owned by the caller and must outlive
// the program.
void setProtected(const char* const* paths, size_t count);
bool isProtected(const char* sanitized_path);

} // namespace PathPolicy
} // namespace sd_storage
