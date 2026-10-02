#pragma once

#include <ArduinoJson.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Mcp {

// Set of 15 built-in tool names defined in gemini_skills_schema.json
const std::unordered_set<std::string>& builtInToolNames();

// Validates tool name format: ^[A-Za-z_][A-Za-z0-9_.-]{0,63}$
bool isValidToolName(std::string_view name);

// Validates URL according to security policy:
// https always allowed; plain http allowed only for private LAN hosts
// (10.x, 192.168.x, 172.16-31.x, 127.x, localhost, *.local).
bool isUrlAllowed(const std::string& url, std::string* error = nullptr);

// Extracts JSON-RPC response body from either application/json or text/event-stream format.
// If expected_id >= 0, searches for the response matching that ID.
std::string extractJsonRpcBody(std::string_view response, int expected_id = -1);

struct ConvertedTool {
    std::string name;
    std::string description;
    std::string declaration_json; // Serialized {"name":..., "description":..., "parametersJsonSchema":...}
};

struct ConvertToolsResult {
    std::vector<ConvertedTool> tools;
    std::vector<std::string> skipped;
    size_t total_declaration_bytes = 0;
};

// Converts tools/list result to Gemini function declarations with parametersJsonSchema.
// Enforces tool name validation, built-in collision prevention, max_tools limit, and ~24 KB cap.
ConvertToolsResult convertToolsListToDeclarations(
    std::string_view json_rpc_result,
    uint8_t max_tools,
    const std::unordered_set<std::string>& built_in_tools = builtInToolNames()
);

// The structuredContent of a device-only tool's tools/call result (tools
// marked _meta "nexus/device_only", which the device calls itself). False,
// with the message, on a JSON-RPC error or an isError result.
bool parseDeviceToolResult(std::string_view json_rpc_result, JsonDocument& out, std::string* error = nullptr);

// Formats a tools/call result into the JSON object expected by Gemini Live.
// Joins text content into {"result": "..."}, includes structuredContent if present,
// and maps isError to {"status": "error", "message": "..."}.
std::string formatToolResponseForGemini(std::string_view json_rpc_result);

} // namespace Mcp
