#pragma once

#include <string>

// Secrets kept in NVS, never in files on the SD card.
//   creds/gemini_api_key          Gemini API key
//   wifi_store/ssid, /password    station credentials
// Values are never logged.
namespace credentials {

std::string geminiApiKey();  // empty if none is stored
bool hasGeminiApiKey();
bool setGeminiApiKey(const std::string& key);

struct Wifi {
    std::string ssid;
    std::string password;
};
bool loadWifi(Wifi& out);  // false if no SSID is stored
bool saveWifi(const Wifi& wifi);
bool clearWifi();

// Moves secrets found on the SD card into NVS, then removes them from the card:
//   /sdcard/wifi_config.json    {"ssid","password"}: imported, file deleted
//   /sdcard/gemini_config.json  "api_key": imported, key removed from the file
//                               (model, voice and system_prompt stay)
//   /sdcard/settings.txt        wifi.* and mqtt.* lines dropped (unused)
// A value is removed from the card only after it reads back from NVS. Call
// after NVS is initialised and the SD card is mounted; this is also how a
// fresh device is provisioned (drop the JSON file on the card and boot).
void importFromSdCard();

}  // namespace credentials
