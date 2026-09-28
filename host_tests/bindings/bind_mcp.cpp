#include "bindings.h"
#include "services/mcp/McpUtil.h"

#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

void init_mcp(nb::module_& m) {
    m.def("mcp_is_valid_tool_name", [](const std::string& name) {
        return Mcp::isValidToolName(name);
    });

    m.def("mcp_is_url_allowed", [](const std::string& url) {
        std::string err;
        bool ok = Mcp::isUrlAllowed(url, &err);
        return std::make_tuple(ok, err);
    });

    m.def("mcp_extract_json_rpc_body", [](const std::string& response, int expected_id) {
        return Mcp::extractJsonRpcBody(response, expected_id);
    });

    m.def("mcp_convert_tools_list", [](const std::string& json_rpc, int max_tools) {
        auto res = Mcp::convertToolsListToDeclarations(json_rpc, static_cast<uint8_t>(max_tools));
        std::vector<std::tuple<std::string, std::string, std::string>> tools;
        for (const auto& t : res.tools) {
            tools.emplace_back(t.name, t.description, t.declaration_json);
        }
        return std::make_tuple(tools, res.skipped, res.total_declaration_bytes);
    });

    m.def("mcp_format_tool_response", [](const std::string& json_rpc) {
        return Mcp::formatToolResponseForGemini(json_rpc);
    });
}
