#pragma once

// Onboard ES8311/ES7210 codec + I2S_NUM_1 clock (mic capture shares this bus).
#define LOCAL_SAMPLE_RATE 32000

// Internal mixer domain rate: Voice/Alert/Media tracks are all produced and
// mixed at this rate. It equals the codec rate so the mixer output goes to
// I2S without a second resample (was 44.1 kHz: Opus 48k -> 44.1k -> 32k).
#define MIXER_SAMPLE_RATE LOCAL_SAMPLE_RATE
