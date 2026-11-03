#pragma once

#include "tool_registry/ToolRegistry.h"
#include <ArduinoJson.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

/**
 * @brief The voice backend a session talks to.
 *
 * Today that is GeminiProtocol, straight to Gemini Live; a hub relay can be
 * another. Everything around it is the same for every backend: wake word,
 * chimes, mic and speaker, AssistantService's state machine, the tools.
 *
 * A backend:
 * - reacts to assistant.session_state in SysDb (connects when a session
 *   starts) and reports assistant.ws_state;
 * - writes the reply audio (16-bit mono PCM, 24 kHz) to VOICE_RX_BUF and sets
 *   audio.assistant_speaking / turn_complete_pending;
 * - hands each tool call to dispatchToolCall(), and sends the answer when
 *   sendToolResponse() is called (from any task).
 *
 * The rest of the firmware reaches the backend through active() only. The
 * backend is chosen when a session starts (selectForSession()); a backend
 * that isn't active() leaves the session alone.
 */
class VoiceAgent {
public:
    virtual ~VoiceAgent() = default;

    // The backend in use. Set by main before the services start, then by
    // selectForSession().
    static VoiceAgent& active();
    static void setActive(VoiceAgent& agent);
    bool isActive() const;
    // Picks the backend for a session (main: the voice_backend setting and
    // whether the hub is up). The wake (AudioService::onWakeWord, every
    // session's start) calls selectForSession() just before it asks for the
    // connection, so the choice never changes during a session. It runs on
    // the wake word task: the selector must be quick and use little stack.
    typedef VoiceAgent& (*SelectorFn)(void* ctx);
    static void setSelector(SelectorFn selector, void* ctx);
    static void selectForSession();
    // Short name for logs ("direct", "hub").
    virtual const char* name() const = 0;

    // Mic audio, 16-bit mono PCM at 16 kHz, while a session streams. Dropped
    // when the backend can't take it (not ready; the reply is playing and
    // barge-in is off).
    virtual void sendMicAudio(const uint8_t* pcm, size_t len) = 0;
    // Sends `text` as a user turn once the session is ready: now if it is,
    // else when it becomes ready. Dropped if the connection closes first.
    virtual void sendTextTurn(const std::string& text) = 0;
    // Answers the tool call `call_id` with a JSON object.
    virtual void sendToolResponse(const char* call_id, const char* json_result) = 0;

    // Connected, and the backend accepted the session's setup: mic audio
    // and text turns go out now.
    virtual bool ready() = 0;
    // A reply is owed and hasn't started (a text turn sent, the person's
    // speech heard, a tool answered): the silence timeout must not end the
    // session meanwhile.
    virtual bool awaitingReply() = 0;
    // A tool the model called hasn't answered yet (the board's, MCP, or one
    // the hub runs), for at most TOOL_WAIT_MS: the session stays open
    // through it even if the model spoke and ended its turn meanwhile ("let
    // me check"). Its answer then makes a reply owed.
    bool toolRunning();
    static constexpr uint32_t TOOL_WAIT_MS = 60000;
    // The person talked over the reply: stop it now.
    virtual void interruptReply() = 0;
    // Barge-in: mic audio goes up while a reply plays.
    virtual void setBargeIn(bool on) = 0;
    // Model turns finished since boot; a tool can tell whether the model
    // spoke (and the user could answer) between two of its calls.
    virtual uint32_t turnsCompleted() const = 0;
    // The session is over (AssistantService). The backend may keep its
    // connection for a quick next session.
    virtual void endSession() = 0;

    // Conversations. A backend resumes a conversation, or keeps its
    // connection for the next session, only within one conversation
    // generation; newConversation() starts the next one, for a setup that
    // changed (another assistant profile). Any task.
    static void newConversation() { s_conversation.fetch_add(1); }
    static uint32_t conversation() { return s_conversation.load(); }

    // The backend heard words from the person (input transcription text):
    // any backend calls this on each one. lastInputWordsUs() is when that
    // last was (esp_timer time, 0 = never since boot).
    static void noteInputWords();
    static int64_t lastInputWordsUs();

    // Tools: every call goes to the registry (main wires it, before the
    // first session). Shared by every backend.
    static void setToolRegistry(tools::Registry* registry) { s_tools = registry; }
    static tools::Registry* toolRegistry() { return s_tools; }

    // Why a session failed: the close code (-1: none, the connection
    // dropped) and reason as the backend saw them. main points it at
    // VoiceSession, which sorts them into kinds. Any task; keep it quick.
    typedef void (*ErrorSinkFn)(int code, const char* reason, void* ctx);
    static void setErrorSink(ErrorSinkFn sink, void* ctx) { s_error_sink = sink; s_error_ctx = ctx; }

protected:
    // A tool the backend's far side runs (the hub's own tools).
    virtual bool remoteToolRunning() { return false; }
    // One tool call from the model (tools::Registry::dispatch): every call
    // gets an answer. True if a handler took the call (its answer, and then
    // the model's reply, are owed).
    bool dispatchToolCall(const char* call_id, const char* name, JsonObjectConst args);
    // The session failed (not a recovery the backend handles itself).
    static void reportSessionError(int code, const char* reason) {
        if (s_error_sink) s_error_sink(code, reason ? reason : "", s_error_ctx);
    }

private:
    static inline std::atomic<uint32_t> s_conversation{0};
    static inline tools::Registry* s_tools = nullptr;
    static inline ErrorSinkFn s_error_sink = nullptr;
    static inline void* s_error_ctx = nullptr;
};
