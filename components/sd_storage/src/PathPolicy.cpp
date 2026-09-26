#include "sd_storage/PathPolicy.h"

#include <cstring>
#include <strings.h>

namespace sd_storage {
namespace PathPolicy {

namespace {
const char* const* s_protected = nullptr;
size_t s_protected_count = 0;

// FAT 8.3 alias of a long name: first 6 characters of the base name, then
// "~N" (GEMINI~1.JSO). Anything with a '~' that starts like a protected file
// is treated as that file.
bool matchesShortAlias(const char* path, const char* protected_path) {
    if (strchr(path, '~') == nullptr) return false;
    const char* slash = strrchr(protected_path, '/');
    size_t dir_len = slash ? static_cast<size_t>(slash - protected_path) + 1 : 0;
    size_t base_len = strlen(protected_path + dir_len);
    size_t prefix_len = dir_len + (base_len < 6 ? base_len : 6);
    return strncasecmp(path, protected_path, prefix_len) == 0;
}
} // namespace

bool sanitize(const char* in_path, std::string& out_path, const char* base_mount) {
    if (base_mount == nullptr || base_mount[0] == '\0') {
        base_mount = "/sdcard";
    }
    if (in_path == nullptr || in_path[0] == '\0') {
        out_path = base_mount;
        return true;
    }

    std::string raw = in_path;
    // Disallow directory traversal
    if (raw.find("..") != std::string::npos) {
        return false;
    }

    for (char& c : raw) {
        if (c == '\\') c = '/';
    }

    std::string normalized;
    normalized.reserve(raw.size());
    bool last_was_slash = false;
    for (char c : raw) {
        if (c == '/') {
            if (!last_was_slash) {
                normalized.push_back(c);
                last_was_slash = true;
            }
        } else {
            normalized.push_back(c);
            last_was_slash = false;
        }
    }

    size_t base_len = strlen(base_mount);
    if (normalized.compare(0, base_len, base_mount) != 0) {
        if (normalized.empty() || normalized[0] != '/') {
            out_path = std::string(base_mount) + "/" + normalized;
        } else {
            out_path = std::string(base_mount) + normalized;
        }
    } else {
        out_path = normalized;
    }

    while (out_path.size() > base_len && out_path.back() == '/') {
        out_path.pop_back();
    }

    return true;
}

void setProtected(const char* const* paths, size_t count) {
    s_protected = paths;
    s_protected_count = count;
}

bool isProtected(const char* path) {
    if (path == nullptr) return true;
    // "." segments reach the same file under another spelling.
    size_t len = strlen(path);
    if (strstr(path, "/./") != nullptr || (len >= 2 && strcmp(path + len - 2, "/.") == 0)) {
        return true;
    }
    for (size_t i = 0; i < s_protected_count; ++i) {
        if (strcasecmp(path, s_protected[i]) == 0 || matchesShortAlias(path, s_protected[i])) {
            return true;
        }
    }
    return false;
}

} // namespace PathPolicy
} // namespace sd_storage
