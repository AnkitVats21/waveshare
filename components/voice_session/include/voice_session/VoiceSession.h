#pragma once

#include "voice_session/SessionRules.h"
#include "tool_registry/ToolRegistry.h"
#include <mutex>

class EmbeddedSysDb;

/**
 * @brief The one owner of what a voice session does after a reply.
 *
 * Others tell it what happened: tool ends (it observes the tool registry),
 * the hub's reports (HubAgent). It applies the rules (SessionRules) and
 * publishes the decision: assistant.end_after_reply in SysDb (read by
 * AssistantService and AudioService) and decision() for the follow-up
 * window. AssistantService resets it when a session starts or ends.
 *
 * Any task: the facts are behind a mutex; the SysDb write happens outside it.
 */
class VoiceSession {
public:
    explicit VoiceSession(EmbeddedSysDb& sysdb) : m_sysdb(sysdb) {}

    // Observe the registry's tool ends. Once, at startup.
    void attach(tools::Registry& registry);
    void setRules(const voice::Rules& rules);

    // The hub carried out a music command for this session.
    void onHubMusicDone();
    // A session starts, or has ended: forget the facts.
    void reset();

    voice::AfterReply decision() const;

private:
    static void onToolEvent(const tools::Event& event, void* ctx);
    void update();

    EmbeddedSysDb& m_sysdb;
    mutable std::mutex m_mutex;
    voice::SessionFacts m_facts;
    voice::Rules m_rules;
};
