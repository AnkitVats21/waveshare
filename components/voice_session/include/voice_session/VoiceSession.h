#pragma once

#include "voice_session/SessionRules.h"
#include "tool_registry/ToolRegistry.h"
#include "esp_timer.h"
#include <mutex>

class EmbeddedSysDb;

/**
 * @brief The one owner of what a voice session does after a reply.
 *
 * Others tell it what happened: tool starts and ends (it observes the tool
 * registry), the hub's reports (HubAgent). It applies the rules (SessionRules) and
 * publishes the decision: assistant.end_after_reply in SysDb (read by
 * AssistantService and AudioService) and decision() for the follow-up
 * window. AssistantService resets it when a session starts or ends.
 *
 * Errors: a failed session's close code and reason are sorted into a kind
 * (SessionRules) and published as assistant.error; after a quota error the
 * next sessions use the fallback model for a while (modelFor()).
 *
 * Tool progress: while a tool runs for longer than PROGRESS_DELAY_MS it
 * publishes assistant.tool and assistant.tool_kind (the LED spins, the
 * dashboard says "Searching…"); cleared when the tool ends.
 *
 * Any task: the facts are behind a mutex; the SysDb write happens outside it.
 */
class VoiceSession {
public:
    explicit VoiceSession(EmbeddedSysDb& sysdb) : m_sysdb(sysdb) {}
    ~VoiceSession();

    static constexpr uint32_t PROGRESS_DELAY_MS = 400;

    // Observe the registry's tool ends. Once, at startup.
    void attach(tools::Registry& registry);
    void setRules(const voice::Rules& rules);

    // The hub carried out a music command for this session.
    void onHubMusicDone();
    // A tool runs on the hub (from reply_owed) / its answer is playing or it
    // gave up.
    void onHubToolStarted(const char* name, const char* kind);
    void onHubToolEnded();
    // The session failed (VoiceAgent's error sink): publishes the kind as
    // assistant.error; a quota error turns the fallback model on.
    void onSessionError(int code, const char* reason);
    // The model for the next session (main's settings source).
    std::string modelFor(const std::string& primary) const;
    // A session starts (new_session: also clears the last error), or has
    // ended: forget the facts.
    void reset(bool new_session);

    voice::AfterReply decision() const;

private:
    static void onToolEvent(const tools::Event& event, void* ctx);
    static void onProgressTimer(void* ctx);
    void update();
    // Called with m_mutex held: (re)arms or clears the progress display.
    void progressChangedLocked();
    void publishTool(const char* name, const char* kind);

    EmbeddedSysDb& m_sysdb;
    mutable std::mutex m_mutex;
    voice::SessionFacts m_facts;
    voice::Rules m_rules;

    int m_tools_running = 0;          // registry tools started, not ended
    bool m_hub_tool = false;
    char m_tool_name[33] = {};        // the latest started
    const char* m_tool_kind = "other";
    bool m_shown = false;             // assistant.tool is set
    esp_timer_handle_t m_timer = nullptr;
    int64_t m_fallback_until_us = 0;  // the fallback model is on until then
};
