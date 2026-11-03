#pragma once

#include <cstring>

// Copies src into the fixed-size buffer dst, cut short if need be, but
// never in the middle of a UTF-8 character: a title cut half-way through a
// multi-byte character is invalid UTF-8, and browsers drop a WebSocket text
// frame that holds one. Always NUL-terminates.
template <size_t N>
inline void copyUtf8(char (&dst)[N], const char* src) {
    static_assert(N > 0, "empty buffer");
    size_t n = strnlen(src, N - 1);
    if (n == N - 1 && src[n] != '\0') {
        // src[n] is the first byte left out: if it continues a character,
        // leave out that whole character.
        while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}
