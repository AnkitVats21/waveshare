#pragma once

#include "tool_registry/skills_generated.h"
#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>
#include <mutex>

/**
 * @brief The tools the voice model can call, and the one path every call and
 * every answer takes.
 *
 * A feature registers its tools (name, kind, handler) at startup; the voice
 * backend hands each call to dispatch() and the setup's declarations come
 * from declareLocal() / declareRemote(). Names that aren't registered go to
 * the remote source (the MCP server). Every answer, now or later, goes out
 * through answer(), so the registry sees each tool start and end and tells
 * the observer (with the answer's status).
 *
 * Portable C++ (no ESP-IDF headers): routing is tested on the host.
 * Registration happens before the first session; dispatch and answer may run
 * on any task.
 */
namespace tools {

enum class Kind : uint8_t { Other, Search, Music, Bluetooth, Info, Device };
const char* kindName(Kind kind);

// The "status" field of an answer: "success" (or none), "ambiguous",
// "not_found", anything else is an error.
enum class Status : uint8_t { Success, Ambiguous, NotFound, Error };
const char* statusName(Status status);
Status statusOf(const char* json_answer);

class Registry;

struct Call {
    const char* id;
    const char* name;
    JsonObjectConst args;
    // The decoded arguments of a tool from the schema; nullptr for a tool
    // with its own declaration.
    const GeminiSkills::DecodedSkillCall* skill;
    // To answer later (Answer::Later): registry->answer(id, json). Copy the id.
    Registry* registry;
};

enum class Answer { Now, Later };
// Fills `reply` and returns Now, or returns Later and answers through
// call.registry->answer() from another task (a slow lookup).
typedef Answer (*Handler)(const Call& call, JsonDocument& reply, void* ctx);

struct Tool {
    const char* name;                 // static storage
    Kind kind;
    Handler handle;
    void* ctx;
    // A function declaration (JSON object); nullptr: the schema's.
    const char* declaration = nullptr;
};

// The remote tools (MCP). call() returns true if it took the call (it then
// answers through registry.answer()).
struct Remote {
    bool (*call)(Registry& registry, const char* id, const char* name, JsonObjectConst args, void* ctx) = nullptr;
    // Adds the remote declarations to `out`, leaving out the name `skip`.
    void (*declare)(JsonArray& out, const char* skip, void* ctx) = nullptr;
    Kind (*kind)(const char* name, void* ctx) = nullptr;
    void* ctx = nullptr;
};

struct Event {
    enum class Type : uint8_t { Started, Ended } type;
    const char* id;
    const char* name;
    Kind kind;
    bool remote;
    Status status;        // Ended only
    uint32_t elapsed_ms;  // Ended only
};
// Runs on the task that dispatched or answered: keep it quick.
typedef void (*Observer)(const Event& event, void* ctx);
// Sends an answer to the model (the active voice backend).
typedef void (*Responder)(const char* id, const char* json, void* ctx);
// Monotonic milliseconds, for elapsed_ms.
typedef uint32_t (*Clock)();

class Registry {
public:
    static constexpr size_t MAX_TOOLS = 32;
    static constexpr size_t MAX_IN_FLIGHT = 8;
    static constexpr size_t MAX_OBSERVERS = 4;

    // False if the table is full or the name is taken.
    bool add(const Tool& tool);
    void setRemote(const Remote& remote) { m_remote = remote; }
    void setResponder(Responder responder, void* ctx) { m_responder = responder; m_responder_ctx = ctx; }
    bool addObserver(Observer observer, void* ctx);
    void setClock(Clock clock) { m_clock = clock; }

    const Tool* find(const char* name) const;
    size_t size() const { return m_count; }

    // One call from the model. Every call gets an answer, or the model waits
    // on it: an unknown name or bad arguments is answered with an error here.
    // True if a handler took the call (its answer, and then the model's
    // reply, are owed).
    bool dispatch(const char* id, const char* name, JsonObjectConst args);
    // Sends the answer to call `id` and reports the end. Any task.
    void answer(const char* id, const char* json);
    // A call is still waiting for its answer (younger than max_age_ms: a
    // worker that never answers doesn't hold a session forever).
    bool busy(uint32_t max_age_ms) const;

    // Declarations of the registered tools, in schema order, then the ones
    // with their own declaration. `skip`: a name to leave out (nullable).
    // `scratch`: holds the parsed schema; the caller picks its allocator.
    bool declareLocal(JsonArray& out, JsonDocument& scratch, const char* skip = nullptr) const;
    void declareRemote(JsonArray& out, const char* skip = nullptr) const;

private:
    struct InFlight {
        char id[64];        // "" = free slot
        char name[48];
        Kind kind;
        bool remote;
        uint32_t start_ms;
    };

    void begin(const char* id, const char* name, Kind kind, bool remote);
    void reply(const char* id, const char* json);
    void notify(const Event& event);
    uint32_t now() const { return m_clock ? m_clock() : 0; }

    Tool m_tools[MAX_TOOLS] = {};
    size_t m_count = 0;
    Remote m_remote;
    Responder m_responder = nullptr;
    void* m_responder_ctx = nullptr;
    struct ObserverSlot { Observer fn; void* ctx; };
    ObserverSlot m_observers[MAX_OBSERVERS] = {};
    size_t m_observer_count = 0;
    Clock m_clock = nullptr;

    mutable std::mutex m_mutex;   // m_in_flight
    InFlight m_in_flight[MAX_IN_FLIGHT] = {};
};

} // namespace tools
