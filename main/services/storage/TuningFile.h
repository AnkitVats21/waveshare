#pragma once

// Loads /sdcard/tuning.json into core_sysdb's Tuning flags (Tuning.h):
// a flat JSON object of integer (or true/false) values, e.g.
//   {"ww_detect_core": 1, "ww_detect_prio": 5}
// Call once at boot, after the card is mounted and before the services that
// read the flags start. A missing file leaves every flag at its default.
namespace Services {

constexpr const char* TUNING_FILE = "/sdcard/tuning.json";

void loadTuningFile();

}  // namespace Services
