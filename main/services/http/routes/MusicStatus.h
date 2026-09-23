#pragma once

#include <ArduinoJson.h>

namespace MusicStatus {

// Player state, position and current track, shared by /api/music/status and
// /api/system/delta (the former adds the queue).
void fill(JsonObject out);

} // namespace MusicStatus
