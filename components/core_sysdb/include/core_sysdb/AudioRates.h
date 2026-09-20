#pragma once

// Onboard ES8311/ES7210 codec + I2S_NUM_1 clock (mic capture shares this bus).
#define LOCAL_SAMPLE_RATE 32000

// Internal mixer domain rate: Voice/Alert/Media tracks are mixed at this
// rate before being downsampled to LOCAL_SAMPLE_RATE for the onboard codec.
#define MIXER_SAMPLE_RATE 44100
