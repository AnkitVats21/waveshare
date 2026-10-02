#pragma once

#include "gemini_skills_generated.h"
#include <ArduinoJson.h>
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
 * The rest of the firmware reaches the backend through active() only.
 */
class VoiceAgent {
public:
    virtual ~VoiceAgent() = default;

    // The backend in use. Set once by main before the services start.
    static VoiceAgent& active();
    static void setActive(VoiceAgent& agent);

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

    // Tools. The board's own skills (gemini_skills_schema.json) go to the
    // tool handler; any other name to the remote handler (MCP), whose
    // declarations come from the declarations source.
    typedef void (*ToolCallHandlerFn)(const GeminiSkills::DecodedSkillCall& skill_call, void* ctx);
    // True if the call was accepted (it answers through sendToolResponse).
    typedef bool (*RemoteToolCallHandlerFn)(const char* call_id, const char* name, JsonObjectConst args, void* ctx);
    // builtin_search: the backend has a search of its own, so a remote search
    // tool would duplicate it.
    typedef void (*RemoteToolsDeclarationsFn)(JsonArray& functionDeclarations, bool builtin_search, void* ctx);

    void setToolCallHandler(ToolCallHandlerFn handler, void* ctx) {
        m_tool_handler = handler;
        m_tool_ctx = ctx;
    }
    void setRemoteToolCallHandler(RemoteToolCallHandlerFn handler, void* ctx) {
        m_remote_tool_handler = handler;
        m_remote_tool_ctx = ctx;
    }
    void setRemoteToolsDeclarationsSource(RemoteToolsDeclarationsFn source, void* ctx) {
        m_remote_decls_source = source;
        m_remote_decls_ctx = ctx;
    }

protected:
    // One tool call from the model. Every call gets a response, or the model
    // waits on it: an unknown name or bad arguments is answered with an
    // error here. True if a handler took the call (its answer, and then the
    // model's reply, are owed). One task at a time (the backend's parser).
    bool dispatchToolCall(const char* call_id, const char* name, JsonObjectConst args);
    // The remote (MCP) tools' declarations, for the backend's setup.
    void addRemoteToolDeclarations(JsonArray& functionDeclarations, bool builtin_search) {
        if (m_remote_decls_source) m_remote_decls_source(functionDeclarations, builtin_search, m_remote_decls_ctx);
    }

private:
    ToolCallHandlerFn m_tool_handler = nullptr;
    void* m_tool_ctx = nullptr;
    RemoteToolCallHandlerFn m_remote_tool_handler = nullptr;
    void* m_remote_tool_ctx = nullptr;
    RemoteToolsDeclarationsFn m_remote_decls_source = nullptr;
    void* m_remote_decls_ctx = nullptr;
    GeminiSkills::DecodedSkillCall m_skill_slot;   // the call being dispatched
};
