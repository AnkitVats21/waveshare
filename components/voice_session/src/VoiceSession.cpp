#include "voice_session/VoiceSession.h"
#include "core_sysdb/EmbeddedSysDb.h"
#include "esp_log.h"

static const char* TAG = "VoiceSession";

void VoiceSession::attach(tools::Registry& registry) {
    registry.addObserver(onToolEvent, this);
}

void VoiceSession::setRules(const voice::Rules& rules) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_rules = rules;
}

void VoiceSession::onToolEvent(const tools::Event& event, void* ctx) {
    if (event.type != tools::Event::Type::Ended) return;
    auto* self = static_cast<VoiceSession*>(ctx);
    const bool ok = event.status == tools::Status::Success;
    {
        std::lock_guard<std::mutex> lock(self->m_mutex);
        auto bump = [](uint8_t& n) { if (n < 255) ++n; };
        if (!event.remote && event.kind == tools::Kind::Music) {
            bump(ok ? self->m_facts.music_ok : self->m_facts.music_failed);
        } else {
            bump(ok ? self->m_facts.tools_ok : self->m_facts.tools_failed);
        }
    }
    self->update();
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
