#pragma once

#include "core_sysdb/app_types.h"

// Names used for assistant and connection state in the API and /api/ws pushes.

inline const char* assistantStateToString(AssistantState s) {
    switch (s) {
        case AssistantState::StartingSession:    return "starting";
        case AssistantState::Connecting:         return "connecting";
        case AssistantState::StreamingUserAudio: return "listening";
        case AssistantState::AssistantSpeaking:  return "speaking";
        case AssistantState::WaitingForFollowup: return "followup";
        case AssistantState::Closing:            return "closing";
        case AssistantState::ErrorCooldown:      return "error";
        default:                                 return "idle";
    }
}

inline const char* wsStateToString(WsState s) {
    switch (s) {
        case WsState::CONNECTING:  return "connecting";
        case WsState::CONNECTED:   return "connected";
        case WsState::GOING_AWAY:  return "going_away";
        case WsState::ERROR_STATE: return "error";
        default:                   return "disconnected";
    }
}
