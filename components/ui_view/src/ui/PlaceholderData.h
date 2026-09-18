#pragma once

// Every hardcoded/fake value shown in the dashboard UI lives here, in one
// obviously-fake place, each with a comment explaining why it's fake and
// what a real data source would need to provide it. None of this is read
// from UiSnapshot/IUiDataSource -- when a real backing field exists, move
// the corresponding dashboard code to read it from the snapshot instead
// and delete the entry here.

#include <lvgl.h>

#include <string>
#include <vector>

namespace lvgl_sim::ui::placeholder {

// ---- Network page ----

// The real device is a two-board system: this ESP32-S3 host talks to a
// companion ESP32-WROOM (Bluetooth A2DP source) over a control UART.
// There's no live link-status field surfaced in UiSnapshot yet -- a real
// value needs the UART link layer to expose a "last heartbeat ok" bit
// through EmbeddedSysDb -> UiSnapshot.
constexpr bool kCompanionBoardLinked = true;
constexpr const char* kCompanionBoardLabel = "ESP32-WROOM (A2DP)";

// ---- Ambient page ----

// No weather integration exists yet; would need a backend HTTP fetch
// (e.g. a weather API proxied through the assistant service) surfaced as
// a new UiSnapshot field.
constexpr const char* kWeatherConditionText = "Partly cloudy";
constexpr const char* kWeatherTempText = "24 C";

// Assistant model/voice: these are configured server-side today but not
// echoed back into UiSnapshot. Real values need a small addition to
// whatever config/status the assistant backend already tracks.
constexpr const char* kAssistantModelText = "Gemini 2.5 Flash";
constexpr const char* kAssistantVoiceText = "Kore";

// ---- Controls page ----

// LED color has no backing store/read-back either -- the dashboard's LED
// swatch just remembers whatever was last applied via LedColorSheet
// in-process; nothing is sent to real hardware yet.

// ---- AssistantScreen ----

struct TranscriptEntry {
    std::string speaker; // "You" or "Assistant"
    std::string text;
};

// Sample conversation, purely for demoing TranscriptList until a real
// transcript event stream exists.
inline const std::vector<TranscriptEntry>& sampleTranscript() {
    static const std::vector<TranscriptEntry> kEntries = {
        {"You", "Hey, what's the weather like today?"},
        {"Assistant", "It's partly cloudy and 24 degrees right now."},
        {"You", "Play something upbeat."},
        {"Assistant", "Sure, starting a playlist for you."},
    };
    return kEntries;
}

} // namespace lvgl_sim::ui::placeholder
