#pragma once

// Onboard ES8311/ES7210 codec + I2S_NUM_1 clock (mic capture shares this bus).
#define LOCAL_SAMPLE_RATE 32000

// Companion I2S link to the ESP32-WROOM Bluetooth board / A2DP output.
// Fixed at 44.1kHz because the companion firmware's SBC negotiation only
// supports 44.1kHz or 48kHz -- no 32kHz option exists there.
#define COMPANION_SAMPLE_RATE 44100
