#pragma once
#include <cstdint>
#include <string>

// What the USB-C port does, chosen at boot from a setting in NVS.
//
// Serial (default): the chip's USB serial/JTAG port, Wi-Fi on, as always.
// Ethernet: the port becomes a USB network adapter (NCM) and Wi-Fi stays
// off. A Linux PC sees a new interface; with its IPv4 method set to
// "Shared to other computers" it hands the board an address and routes it
// to the internet. Used to compare the board's network speed with and
// without its Wi-Fi link.
//
// The USB port can't go back from the network adapter to serial while
// running, so changing the mode saves it and reboots. If no PC hands out an
// address within kFallbackSec of boot, the board saves Serial and reboots,
// so a board unplugged from the PC comes back on Wi-Fi.
namespace UsbNet {

enum class Mode : uint8_t { Serial = 0, Ethernet = 1 };

constexpr uint32_t kFallbackSec = 60;

Mode savedMode();
bool saveMode(Mode mode);
const char* modeName(Mode mode);
bool parseMode(const char* name, Mode& out);

// Starts the USB network interface and its DHCP client. Call instead of
// WifiService::begin(), after esp_netif_init() and NVS.
bool begin();

// The address the PC handed out, or "" before it has.
std::string ip();

}  // namespace UsbNet
