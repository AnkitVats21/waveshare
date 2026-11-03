#include "voice_session/VoiceSession.h"
#include "core_sysdb/EmbeddedSysDb.h"
#include "esp_log.h"

static const char* TAG = "VoiceSession";

#include <cstring>

VoiceSession::~VoiceSession() {
    if (m_timer) {
        esp_timer_stop(m_timer);
        esp_timer_delete(m_timer);
    }
}

void VoiceSession::attach(tools::Registry& registry) {
    registry.addObserver(onToolEvent, this);
    esp_timer_create_args_t args = {};
    args.callback = onProgressTimer;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "tool_progress";
    esp_timer_create(&args, &m_timer);
}

void VoiceSession::setRules(const voice::Rules& rules) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_rules = rules;
}

void VoiceSession::onToolEvent(const tools::Event& event, void* ctx) {
    auto* self = static_cast<VoiceSession*>(ctx);
    if (event.type == tools::Event::Type::Started) {
        std::lock_guard<std::mutex> lock(self->m_mutex);
        self->m_tools_running++;
        strncpy(self->m_tool_name, event.name, sizeof(self->m_tool_name) - 1);
        self->m_tool_name[sizeof(self->m_tool_name) - 1] = '\0';
        self->m_tool_kind = tools::kindName(event.kind);
        self->progressChangedLocked();
        return;
    }
    const bool ok = event.status == tools::Status::Success;
    {
        std::lock_guard<std::mutex> lock(self->m_mutex);
        if (self->m_tools_running > 0) self->m_tools_running--;
        auto bump = [](uint8_t& n) { if (n < 255) ++n; };
        if (!event.remote && event.kind == tools::Kind::Music) {
            bump(ok ? self->m_facts.music_ok : self->m_facts.music_failed);
        } else {
            bump(ok ? self->m_facts.tools_ok : self->m_facts.tools_failed);
        }
        self->progressChangedLocked();
    }
    self->update();
}

void VoiceSession::onHubToolStarted() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hub_tool = true;
    // The hub doesn't say which tool: a generic one.
    strcpy(m_tool_name, "tool");
    m_tool_kind = "other";
    progressChangedLocked();
}

void VoiceSession::onHubToolEnded() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_hub_tool) return;
    m_hub_tool = false;
    progressChangedLocked();
}

void VoiceSession::progressChangedLocked() {
    const bool running = m_tools_running > 0 || m_hub_tool;
    if (!m_timer) return;
    esp_timer_stop(m_timer);
    if (running) {
        // Shown after the delay (a quick tool doesn't flash); a newer tool
        // replaces the name at once if one is already shown.
        esp_timer_start_once(m_timer, m_shown ? 0 : uint64_t(PROGRESS_DELAY_MS) * 1000);
    } else if (m_shown) {
        esp_timer_start_once(m_timer, 0);   // clear, off this task
    }
}

void VoiceSession::onProgressTimer(void* ctx) {
    auto* self = static_cast<VoiceSession*>(ctx);
    char name[33];
    const char* kind;
    bool show;
    {
        std::lock_guard<std::mutex> lock(self->m_mutex);
        show = self->m_tools_running > 0 || self->m_hub_tool;
        memcpy(name, self->m_tool_name, sizeof(name));
        kind = self->m_tool_kind;
        self->m_shown = show;
    }
    self->publishTool(show ? name : "", show ? kind : "");
}

void VoiceSession::publishTool(const char* name, const char* kind) {
    m_sysdb.mutate([name, kind](SystemState& s) {
        if (strcmp(s.assistant.tool, name) == 0 && strcmp(s.assistant.tool_kind, kind) == 0) return;
        strncpy(s.assistant.tool, name, sizeof(s.assistant.tool) - 1);
        s.assistant.tool[sizeof(s.assistant.tool) - 1] = '\0';
        strncpy(s.assistant.tool_kind, kind, sizeof(s.assistant.tool_kind) - 1);
        s.assistant.tool_kind[sizeof(s.assistant.tool_kind) - 1] = '\0';
    });
    if (name[0]) ESP_LOGI(TAG, "Tool running: %s (%s)", name, kind);
}

void VoiceSession::onHubMusicDone() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_facts.hub_music_done = true;
    }
    update();
}

void VoiceSession::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_facts = {};
    // Calls still out belong to the session that ended; their answers go
    // nowhere useful. (A late answer only lowers the count to zero.)
    m_tools_running = 0;
    m_hub_tool = false;
    progressChangedLocked();
}

voice::AfterReply VoiceSession::decision() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return voice::decide(m_facts, m_rules);
}

void VoiceSession::update() {
    const voice::AfterReply d = decision();
    if (!d.end) return;
    // Not in Idle: the decision would leak into the next session.
    bool set = false;
    m_sysdb.mutate([&set](SystemState& s) {
        if (s.assistant.session_state != AssistantState::Idle && !s.assistant.end_after_reply) {
            s.assistant.end_after_reply = true;
            set = true;
        }
    });
    if (set) ESP_LOGI(TAG, "The session ends after this reply.");
}
