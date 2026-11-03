#include "bindings.h"
#include "tool_registry/ToolRegistry.h"

#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <deque>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

uint32_t g_now_ms = 0;

// A registry with scripted tools, recording what goes to the model and
// what the observer sees.
struct ToolHarness {
    struct LocalTool {
        std::string name;
        std::string reply;   // answered now with this JSON; empty = later
        std::string seen_args;
        bool decoded = false;
    };

    tools::Registry registry;
    std::deque<LocalTool> locals;   // stable addresses (ctx, name)
    std::set<std::string> remote_names;
    std::vector<std::tuple<std::string, std::string>> responses;            // (id, json)
    std::vector<std::tuple<std::string, std::string, std::string, bool, std::string, uint32_t>> events;
    std::vector<std::string> remote_calls;

    ToolHarness() {
        registry.setClock([]() { return g_now_ms; });
        registry.setResponder([](const char* id, const char* json, void* ctx) {
            static_cast<ToolHarness*>(ctx)->responses.emplace_back(id, json);
        }, this);
        registry.addObserver([](const tools::Event& e, void* ctx) {
            static_cast<ToolHarness*>(ctx)->events.emplace_back(
                e.type == tools::Event::Type::Started ? "started" : "ended", e.name, tools::kindName(e.kind),
                e.remote, e.type == tools::Event::Type::Ended ? tools::statusName(e.status) : "", e.elapsed_ms);
        }, this);
    }

    bool add(const std::string& name, int kind, const std::string& reply, const std::string& declaration) {
        locals.push_back({name, reply, "", false});
        LocalTool& t = locals.back();
        static std::deque<std::string> decls;
        const char* decl = nullptr;
        if (!declaration.empty()) {
            decls.push_back(declaration);
            decl = decls.back().c_str();
        }
        return registry.add({t.name.c_str(), static_cast<tools::Kind>(kind),
                             [](const tools::Call& call, JsonDocument& out, void* ctx) {
                                 auto& tool = *static_cast<LocalTool*>(ctx);
                                 tool.decoded = call.skill != nullptr;
                                 tool.seen_args.clear();
                                 serializeJson(call.args, tool.seen_args);
                                 if (tool.reply.empty()) return tools::Answer::Later;
                                 deserializeJson(out, tool.reply);
                                 return tools::Answer::Now;
                             },
                             &t, decl});
    }

    void setRemote(const std::vector<std::string>& names) {
        remote_names = std::set<std::string>(names.begin(), names.end());
        tools::Remote r;
        r.call = [](tools::Registry&, const char*, const char* name, JsonObjectConst, void* ctx) {
            auto& h = *static_cast<ToolHarness*>(ctx);
            if (!h.remote_names.count(name)) return false;
            h.remote_calls.push_back(name);
            return true;
        };
        r.declare = [](JsonArray& out, const char* skip, void* ctx) {
            for (const auto& n : static_cast<ToolHarness*>(ctx)->remote_names) {
                if (skip && n == skip) continue;
                out.add<JsonObject>()["name"] = n;
            }
        };
        r.kind = [](const char* name, void*) {
            return std::string(name).find("search") != std::string::npos ? tools::Kind::Search : tools::Kind::Other;
        };
        r.ctx = this;
        registry.setRemote(r);
    }

    bool dispatch(const std::string& id, const std::string& name, const std::string& args) {
        JsonDocument doc;
        deserializeJson(doc, args.empty() ? "{}" : args);
        return registry.dispatch(id.c_str(), name.c_str(), doc.as<JsonObjectConst>());
    }

    std::vector<std::string> declared(const std::string& skip, bool remote) {
        JsonDocument out, scratch;
        JsonArray arr = out.to<JsonArray>();
        registry.declareLocal(arr, scratch, skip.empty() ? nullptr : skip.c_str());
        if (remote) registry.declareRemote(arr, skip.empty() ? nullptr : skip.c_str());
        std::vector<std::string> names;
        for (JsonObjectConst d : arr) names.push_back(d["name"] | "");
        return names;
    }
};

}  // namespace

void init_tools(nb::module_& m) {
    m.def("tools_set_now_ms", [](uint32_t ms) { g_now_ms = ms; });
    m.def("tools_status_of", [](const std::string& json) { return std::string(tools::statusName(tools::statusOf(json.c_str()))); });

    nb::class_<ToolHarness>(m, "ToolHarness")
        .def(nb::init<>())
        .def("add", &ToolHarness::add, nb::arg("name"), nb::arg("kind"), nb::arg("reply") = "",
             nb::arg("declaration") = "")
        .def("set_remote", &ToolHarness::setRemote)
        .def("dispatch", &ToolHarness::dispatch, nb::arg("id"), nb::arg("name"), nb::arg("args") = "{}")
        .def("answer", [](ToolHarness& h, const std::string& id, const std::string& json) {
            h.registry.answer(id.c_str(), json.c_str());
        })
        .def("busy", [](ToolHarness& h, uint32_t max_age_ms) { return h.registry.busy(max_age_ms); })
        .def("declared", &ToolHarness::declared, nb::arg("skip") = "", nb::arg("remote") = true)
        .def("seen_args", [](ToolHarness& h, const std::string& name) {
            for (auto& t : h.locals) if (t.name == name) return std::make_tuple(t.decoded, t.seen_args);
            return std::make_tuple(false, std::string());
        })
        .def_ro("responses", &ToolHarness::responses)
        .def_ro("events", &ToolHarness::events)
        .def_ro("remote_calls", &ToolHarness::remote_calls);
}
