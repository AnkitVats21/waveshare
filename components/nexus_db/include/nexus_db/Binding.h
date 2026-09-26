#pragma once

#include <type_traits>

#include "nexus_db/Fields.h"

namespace nexus_db {

// Copies between a document field and the SystemState field it is bound to
// (sysdb= in the schema). Numbers and enums convert with static_cast; colors
// are any struct with r, g, b members.
template <typename Dst, typename Src>
void assignField(Dst& dst, const Src& src) {
    if constexpr (std::is_same_v<Dst, Rgb> && !std::is_same_v<Src, Rgb>) {
        dst = {src.r, src.g, src.b};
    } else if constexpr (std::is_same_v<Src, Rgb> && !std::is_same_v<Dst, Rgb>) {
        dst.r = src.r;
        dst.g = src.g;
        dst.b = src.b;
    } else {
        dst = static_cast<Dst>(src);
    }
}

} // namespace nexus_db
