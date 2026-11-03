#include "gemini_live/VoiceAgent.h"
#include "core_sysdb/AppLogger.h"
#include "core_sysdb/EmbeddedSysDb.h"
#include <atomic>
#include "esp_timer.h"
#include <cassert>
#include <cstring>
#include <string>

static std::atomic<VoiceAgent*> s_active{nullptr};
static std::atomic<int64_t> s_last_words_us{0};

void VoiceAgent::noteInputWords() { s_last_words_us = esp_timer_get_time(); }
int64_t VoiceAgent::lastInputWordsUs() { return s_last_words_us.load(); }
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
    if (s_tools) return s_tools->dispatch(call_id, name, args);
    LOGW_NET("Tool call '%s' with no tool registry", name ? name : "?");
    sendToolResponse(call_id, "{\"status\":\"error\",\"message\":\"No tools\"}");
    return false;
}

bool VoiceAgent::toolRunning() {
    return (s_tools && s_tools->busy(TOOL_WAIT_MS)) || remoteToolRunning();
}
