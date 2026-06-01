#pragma once

#include "common/events/event_bases.h"
#include "common/led_types.h"
#include <cstdint>

/**
 * @brief Application-level event IDs and their associated payload types.
 *
 * Each event enum value is paired with its payload struct directly below it,
 * so the full contract (ID + data) is visible in one place.
 *
 * Publish:   EventBus::getInstance().publish(APP_EVENTS, AppEvent::XXX, payload)
 * Subscribe: IService::subscribeEvent(APP_EVENTS, AppEvent::XXX)
 */
enum class AppEvent : int32_t {
  WAKE_WORD_DETECTED,      ///< Wake word confirmed; payload: WakeWordData
  STOP_STREAMING,          ///< VAD silence timeout; payload: none (uint32_t zero)
  LED_COMMAND,             ///< LED pattern request; payload: LedEventData
  LED_COLOR_UPDATE,        ///< Direct RGB color set; payload: RgbColor
  ASSISTANT_TALKING,       ///< Server started sending audio; payload: none
  ASSISTANT_SILENT,        ///< Server went quiet mid-turn; payload: none
  ASSISTANT_TURN_COMPLETE, ///< Server signalled end of turn; payload: none
  USER_INTERRUPTED,        ///< User barge-in detected; payload: none
  MIC_GAIN_UPDATE,         ///< Calibrated mic gain available; payload: MicGainData
  STREAMING_STOP_REQUESTED,///< External stop request; payload: none
  GEMINI_TOOL_CALL,        ///< Gemini Live skill request; payload: GeminiSkillPayload
};

// ---------------------------------------------------------------------------
// Payload structs — one per event that carries data
// ---------------------------------------------------------------------------

/** Payload for AppEvent::WAKE_WORD_DETECTED */
struct WakeWordData {
  uint8_t channel; ///< AFE beam-forming channel that triggered the wake word
};

/** Payload for AppEvent::MIC_GAIN_UPDATE */
struct MicGainData {
  float db; ///< Optimal mic gain in dB as determined by calibration
};

// AppEvent::LED_COMMAND uses LedEventData from led_types.h
// All other listed events carry no payload (publish a uint32_t zero).

namespace GeminiSkills { struct DecodedSkillCall; }

/** Payload for AppEvent::GEMINI_TOOL_CALL */
typedef GeminiSkills::DecodedSkillCall* GeminiSkillPayload;
