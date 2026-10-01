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

// Receive-path detail for network tests: frames in, frames lwIP refused
// (its input mailbox was full: lost inside the board) and pauses between
// frames. A pause >= 100 ms during a download means the sender was waiting:
// for a lost packet's resend, or for the board to open its window.
struct RxDetail {
    uint32_t frames;
    uint32_t refused;
    uint32_t gaps_100ms;      // pauses of 100 ms or more
    uint32_t gaps_500ms;      // of which 500 ms or more
    uint32_t gap_total_ms;    // time spent in those pauses
    uint32_t max_gap_ms;
    // IPv4 frames by IP precedence (TOS >> 5), which picks the Wi-Fi
    // priority queue: 0 and 3 best effort, 1-2 background, 4-5 video,
    // 6-7 voice. Since the reset.
    uint32_t precedence[8];
};
// Totals since the last reset; the gap fields start counting at the reset.
RxDetail rxDetail();
void resetRxDetail();

}  // namespace NetStats
