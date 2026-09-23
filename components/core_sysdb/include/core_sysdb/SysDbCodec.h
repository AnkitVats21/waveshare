#pragma once

#include "core_sysdb/SystemState.h"
#include <cstring>

namespace SysDbCodec {

/**
 * @brief Diff two states field by field and return the OR'd (COMP::X | BIT_X::FIELD) change mask.
 */
ComponentMask diffState(const SystemState& old_s, const SystemState& new_s);

} // namespace SysDbCodec
