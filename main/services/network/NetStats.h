#pragma once
#include <cstdint>
#include "esp_netif.h"

// Bytes through the Wi-Fi station interface since boot, counted where lwIP
// hands frames to and from the driver: everything the board sends and
// receives (TLS, headers and all), whichever feature it belongs to.
namespace NetStats {

// Hooks the interface's input and output functions. Call it once the
// station has an address (the functions exist only after Wi-Fi started);
// calling it again is harmless.
void install(esp_netif_t* sta);

// Totals since boot. 32-bit, so they wrap after 4 GB; the reader takes
// differences (unsigned subtraction handles the wrap).
uint32_t rxBytes();
uint32_t txBytes();

}  // namespace NetStats
