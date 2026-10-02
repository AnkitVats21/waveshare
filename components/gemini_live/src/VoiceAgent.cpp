#include "gemini_live/VoiceAgent.h"
#include "common/AppLogger.h"
#include <cassert>
#include <cstring>
#include <string>

static VoiceAgent* s_active = nullptr;

VoiceAgent& VoiceAgent::active() {
    assert(s_active != nullptr);
    return *s_active;
}

void VoiceAgent::setActive(VoiceAgent& agent) {
    s_active = &agent;
}

bool VoiceAgent::dispatchToolCall(const char* call_id, const char* name, JsonObjectConst args) {
    auto& slot = m_skill_slot;
    slot.reset();
    if (!GeminiSkills::decode_incoming_arguments(name, args, slot)) {
        if (m_remote_tool_handler && m_remote_tool_handler(call_id, name, args, m_remote_tool_ctx)) {
            LOGI_NET("Remote MCP tool dispatched: %s", name);
            slot.reset();
            return true;
        }
        LOGW_NET("Rejected tool call '%s': %s", name, slot.error);
        JsonDocument err;
        err["status"] = "error";
        err["message"] = slot.error;
        std::string out;
        serializeJson(err, out);
        sendToolResponse(call_id, out.c_str());
        slot.reset();
        return false;
    }
    std::strncpy(slot.call_id, call_id, sizeof(slot.call_id) - 1);

    LOGI_NET("Tool request: %s", name);
    bool taken = m_tool_handler != nullptr;
    if (taken) {
        m_tool_handler(slot, m_tool_ctx);
    } else {
        sendToolResponse(call_id, "{\"status\":\"error\",\"message\":\"No tool handler\"}");
    }
    slot.reset();
    return taken;
}
