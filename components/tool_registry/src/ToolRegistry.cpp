#include "tool_registry/ToolRegistry.h"
#include "esp_log.h"
#include <cstring>
#include <string>

namespace tools {

static const char* TAG = "tools";

const char* kindName(Kind kind) {
    switch (kind) {
        case Kind::Search: return "search";
        case Kind::Music: return "music";
        case Kind::Bluetooth: return "bluetooth";
        case Kind::Info: return "info";
        case Kind::Device: return "device";
        default: return "other";
    }
}

const char* statusName(Status status) {
    switch (status) {
        case Status::Success: return "success";
        case Status::Ambiguous: return "ambiguous";
        case Status::NotFound: return "not_found";
        default: return "error";
    }
}

Status statusOf(const char* json_answer) {
    if (!json_answer) return Status::Error;
    JsonDocument filter;
    filter["status"] = true;
    filter["error"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, json_answer, DeserializationOption::Filter(filter))) return Status::Error;
    if (!doc["error"].isNull()) return Status::Error;
    const char* s = doc["status"] | "success";
    if (strcmp(s, "success") == 0 || strcmp(s, "ok") == 0) return Status::Success;
    if (strcmp(s, "ambiguous") == 0) return Status::Ambiguous;
    if (strcmp(s, "not_found") == 0) return Status::NotFound;
    return Status::Error;
}

bool Registry::add(const Tool& tool) {
    if (!tool.name || !tool.handle || m_count >= MAX_TOOLS || find(tool.name)) {
        ESP_LOGE(TAG, "Tool '%s' not registered (%s)", tool.name ? tool.name : "?",
                 m_count >= MAX_TOOLS ? "table full" : "duplicate or incomplete");
        return false;
    }
    m_tools[m_count++] = tool;
    return true;
}

bool Registry::addObserver(Observer observer, void* ctx) {
    if (!observer || m_observer_count >= MAX_OBSERVERS) return false;
    m_observers[m_observer_count++] = {observer, ctx};
    return true;
}

const Tool* Registry::find(const char* name) const {
    if (!name) return nullptr;
    for (size_t i = 0; i < m_count; ++i) {
        if (strcmp(m_tools[i].name, name) == 0) return &m_tools[i];
    }
    return nullptr;
}

void Registry::notify(const Event& event) {
    for (size_t i = 0; i < m_observer_count; ++i) m_observers[i].fn(event, m_observers[i].ctx);
}

void Registry::reply(const char* id, const char* json) {
    if (m_responder) m_responder(id, json, m_responder_ctx);
}

void Registry::begin(const char* id, const char* name, Kind kind, bool remote) {
    uint32_t t = now();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        InFlight* slot = nullptr;
        for (auto& f : m_in_flight) {
            if (f.id[0] == '\0') { slot = &f; break; }
        }
        if (!slot) {
            // Calls that never got an answer (a session that ended first):
            // the oldest makes room.
            slot = &m_in_flight[0];
            for (auto& f : m_in_flight) {
                if ((int32_t)(f.start_ms - slot->start_ms) < 0) slot = &f;
            }
        }
        strncpy(slot->id, id, sizeof(slot->id) - 1);
        slot->id[sizeof(slot->id) - 1] = '\0';
        strncpy(slot->name, name, sizeof(slot->name) - 1);
        slot->name[sizeof(slot->name) - 1] = '\0';
        slot->kind = kind;
        slot->remote = remote;
        slot->start_ms = t;
    }
    notify(Event{Event::Type::Started, id, name, kind, remote, Status::Success, 0});
}

bool Registry::dispatch(const char* id, const char* name, JsonObjectConst args) {
    if (!id) id = "";
    if (!name) name = "";

    if (const Tool* tool = find(name)) {
        GeminiSkills::DecodedSkillCall skill;
        const GeminiSkills::DecodedSkillCall* decoded = nullptr;
        if (!tool->declaration) {
            if (!GeminiSkills::decode_incoming_arguments(name, args, skill)) {
                ESP_LOGW(TAG, "Rejected tool call '%s': %s", name, skill.error);
                JsonDocument err;
                err["status"] = "error";
                err["message"] = skill.error;
                std::string out;
                serializeJson(err, out);
                reply(id, out.c_str());
                return false;
            }
            strncpy(skill.call_id, id, sizeof(skill.call_id) - 1);
            decoded = &skill;
        }
        ESP_LOGI(TAG, "Tool call: %s", name);
        begin(id, tool->name, tool->kind, false);
        JsonDocument out;
        Call call{id, name, args, decoded, this};
        if (tool->handle(call, out, tool->ctx) == Answer::Now) {
            std::string json;
            serializeJson(out, json);
            answer(id, json.c_str());
        }
        return true;
    }

    if (m_remote.call) {
        Kind kind = m_remote.kind ? m_remote.kind(name, m_remote.ctx) : Kind::Other;
        // Started before the call: a quick remote answer must find it.
        begin(id, name, kind, true);
        if (m_remote.call(*this, id, name, args, m_remote.ctx)) {
            ESP_LOGI(TAG, "Remote tool call: %s", name);
            return true;
        }
        ESP_LOGW(TAG, "Rejected tool call '%s': unknown tool", name);
        answer(id, "{\"status\":\"error\",\"message\":\"Unknown tool\"}");
        return false;
    }

    ESP_LOGW(TAG, "Rejected tool call '%s': unknown tool", name);
    reply(id, "{\"status\":\"error\",\"message\":\"Unknown tool\"}");
    return false;
}

void Registry::answer(const char* id, const char* json) {
    if (!id) return;
    bool found = false;
    InFlight call{};
    uint32_t t = now();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& f : m_in_flight) {
            if (f.id[0] && strcmp(f.id, id) == 0) {
                call = f;
                f.id[0] = '\0';
                found = true;
                break;
            }
        }
    }
    ESP_LOGI(TAG, "Tool answer %s: %.200s", found ? call.name : id, json ? json : "");
    reply(id, json);
    if (found) {
        notify(Event{Event::Type::Ended, call.id, call.name, call.kind, call.remote, statusOf(json),
                     t - call.start_ms});
    }
}

bool Registry::busy(uint32_t max_age_ms) const {
    uint32_t t = now();
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& f : m_in_flight) {
        if (f.id[0] && t - f.start_ms < max_age_ms) return true;
    }
    return false;
}

bool Registry::declareLocal(JsonArray& out, JsonDocument& scratch, const char* skip) const {
    if (deserializeJson(scratch, GeminiSkills::TOOL_DECLARATIONS_JSON)) {
        ESP_LOGE(TAG, "Tool declarations: out of memory");
        return false;
    }
    for (JsonObjectConst decl : scratch.as<JsonArrayConst>()) {
        const char* name = decl["name"] | "";
        const Tool* tool = find(name);
        if (!tool || tool->declaration || (skip && strcmp(name, skip) == 0)) continue;
        out.add(decl);
    }
    for (size_t i = 0; i < m_count; ++i) {
        const Tool& tool = m_tools[i];
        if (!tool.declaration || (skip && strcmp(tool.name, skip) == 0)) continue;
        scratch.clear();
        if (deserializeJson(scratch, tool.declaration)) {
            ESP_LOGE(TAG, "Bad declaration for tool '%s'", tool.name);
            continue;
        }
        out.add(scratch.as<JsonObjectConst>());
    }
    scratch.clear();
    return true;
}

void Registry::declareRemote(JsonArray& out, const char* skip) const {
    if (m_remote.declare) m_remote.declare(out, skip, m_remote.ctx);
}

} // namespace tools
