#pragma once

#include "common/events/event_bases.h"
#include "app/assistant/AssistantVisualState.h"
#include <cstdint>

// Declare the event base for the assistant system
ESP_EVENT_DECLARE_BASE(ASSISTANT_EVENTS);

/**
 * @brief Input events and output commands for the Assistant Session State Machine.
 */
enum class AssistantEvent : int32_t {
  // Input Events (Triggers to State Machine)
  WAKE_WORD_DETECTED,       ///< Wake word confirmed; payload: AssistantWakeWordData
  WIFI_AVAILABLE,           ///< Network connection ready; payload: none
  WIFI_LOST,                ///< Network connection lost; payload: none
  USER_SPEECH_DETECTED,     ///< User started speaking; payload: none
  VAD_TIMEOUT,              ///< VAD silence threshold hit; payload: none
  USER_INTERRUPTED,         ///< User interrupted assistant; payload: none
  WS_CONNECTED,             ///< WebSocket established; payload: none
  WS_CONNECT_FAILED,        ///< WebSocket connection failed; payload: int32_t reason
  WS_CLOSED,                ///< WebSocket connection closed; payload: int32_t reason
  GEMINI_GO_AWAY,           ///< Server sent goAway; payload: none
  ASSISTANT_AUDIO_STARTED,  ///< Server started sending audio; payload: none
  ASSISTANT_AUDIO_CHUNK,    ///< Server audio chunk received; payload: none
  ASSISTANT_TURN_COMPLETE,  ///< Server signaled turn complete; payload: none
  TOOL_CALL_RECEIVED,       ///< Skill execution request received; payload: none
  QUOTA_EXCEEDED,           ///< Server quota limit hit; payload: none
  SERVER_ERROR,             ///< Server error returned; payload: int32_t code
  TRANSPORT_ERROR,          ///< Transport error returned; payload: int32_t code
  CONNECT_TIMEOUT,          ///< WebSocket connection timed out; payload: none
  SESSION_IDLE_TIMEOUT,     ///< Conversational idle timeout; payload: none
  COOLDOWN_ELAPSED,         ///< Cooldown period elapsed; payload: none

  // Output Commands (Subsystem Directives)
  TRANSPORT_CONNECT,                 ///< Request WebSocket connect
  TRANSPORT_SEND_BUFFERED_AUDIO,     ///< Request sending pre-connect buffer
  TRANSPORT_SEND_LIVE_AUDIO,         ///< Enable live audio upload
  TRANSPORT_CLOSE,                   ///< Request WebSocket close
  AUDIO_ENTER_CONVERSATION_MODE,     ///< Prepare audio pipeline for user speech
  AUDIO_ENTER_PLAYBACK_MODE_24K,     ///< Reconfigure clock and tasks for 24kHz audio playback
  AUDIO_RETURN_TO_WAKE_MODE_16K,     ///< Return clock and tasks to 16kHz VAD wake word mode
  AUDIO_FLUSH_PLAYBACK,              ///< Flush physical speaker play buffers
  AUDIO_RESUME_MIC_STREAMING,        ///< Resume microphone DMA audio capture
  AUDIO_SUSPEND_MIC_STREAMING,       ///< Suspend microphone DMA audio capture
  VISUAL_STATE_CHANGED               ///< Publish high-level visual state change; payload: AssistantVisualState
};

struct AssistantWakeWordData {
  uint8_t channel; ///< AFE beam-forming channel that triggered the wake word
};
