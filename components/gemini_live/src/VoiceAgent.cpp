#include "gemini_live/VoiceAgent.h"
#include "common/AppLogger.h"
#include "common/sysdb/EmbeddedSysDb.h"
#include <atomic>
#include <cassert>
#include <cstring>
#include <string>

static std::atomic<VoiceAgent*> s_active{nullptr};
static VoiceAgent::SelectorFn s_selector = nullptr;
static void* s_selector_ctx = nullptr;

VoiceAgent& VoiceAgent::active() {
    VoiceAgent* agent = s_active.load();
    assert(agent != nullptr);
    return *agent;
}

void VoiceAgent::setActive(VoiceAgent& agent) {
    s_active = &agent;
}

bool VoiceAgent::isActive() const {
    return s_active.load() == this;
}

void VoiceAgent::setSelector(SelectorFn selector, void* ctx) {
    s_selector = selector;
    s_selector_ctx = ctx;
}

void VoiceAgent::selectForSession() {
    if (!s_selector) return;
    VoiceAgent& chosen = s_selector(s_selector_ctx);
    VoiceAgent* previous = s_active.exchange(&chosen);
    if (previous != &chosen) {
        LOGI_NET("Voice backend: %s (was %s)", chosen.name(), previous ? previous->name() : "none");
        // ws_state was the old backend's (a kept connection reads CONNECTED):
        // the new one starts from DISCONNECTED. The old one closes without
        // touching it.
        EmbeddedSysDb::getInstance().mutate([](SystemState& s) {
            s.assistant.ws_state = WsState::DISCONNECTED;
            s.audio.assistant_speaking = false;
        });
    }
}

bool VoiceAgent::dispatchToolCall(const char* call_id, const char* name, JsonObjectConst args) {
    auto& slot = m_skill_slot;
    slot.reset();
    if (!GeminiSkills::decode_incoming_arguments(name, args, slot)) {
        if (s_remote_tool_handler && s_remote_tool_handler(call_id, name, args, s_remote_tool_ctx)) {
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
    bool taken = s_tool_handler != nullptr;
    if (taken) {
        s_tool_handler(slot, s_tool_ctx);
    } else {
        sendToolResponse(call_id, "{\"status\":\"error\",\"message\":\"No tool handler\"}");
    }
    slot.reset();
    return taken;
}
