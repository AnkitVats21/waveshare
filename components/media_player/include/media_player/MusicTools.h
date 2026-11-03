#pragma once

#include "tool_registry/ToolRegistry.h"

// Registers play, playback, volume, music_settings and music_output
// (MusicPlaybackService, the speaker volume).
void registerMusicTools(tools::Registry& registry);
