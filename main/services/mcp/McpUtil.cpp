#include "services/mcp/McpUtil.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace Mcp {

namespace {

constexpr size_t MAX_DECLARATIONS_TOTAL_BYTES = 24 * 1024; // ~24 KB

// Helper to parse an IPv4 address. Returns true if valid IPv4, filling octets.
bool parseIPv4(std::string_view host, int octets[4]) {
    int count = 0;
    int current = 0;
    bool has_digit = false;

    for (size_t i = 0; i < host.size(); ++i) {
        char c = host[i];
        if (c >= '0' && c <= '9') {
            current = current * 10 + (c - '0');
            if (current > 255) return false;
            has_digit = true;
        } else if (c == '.') {
            if (!has_digit || count >= 3) return false;
            octets[count++] = current;
            current = 0;
            has_digit = false;
        } else {
            return false;
        }
    }
    if (!has_digit || count != 3) return false;
    octets[3] = current;
    return true;
}

// Checks if an IPv4 address is in a private RFC1918 or loopback range.
bool isPrivateIPv4(const int octets[4]) {
    // 10.0.0.0/8
    if (octets[0] == 10) return true;
    // 127.0.0.0/8
    if (octets[0] == 127) return true;
    // 192.168.0.0/16
    if (octets[0] == 192 && octets[1] == 168) return true;
    // 172.16.0.0/12
    if (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) return true;
    return false;
}

// Extract host and port from a URL (e.g. "http://192.168.1.10:8788/mcp" -> "192.168.1.10")
std::string extractHost(std::string_view url, bool is_https) {
    size_t prefix_len = is_https ? 8 : 7; // "https://" vs "http://"
    if (url.size() <= prefix_len) return "";

    std::string_view rest = url.substr(prefix_len);
    size_t end = rest.find_first_of("/?#:");
    std::string_view host_part = (end == std::string_view::npos) ? rest : rest.substr(0, end);

    std::string host(host_part);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
    return host;
}

} // namespace

const std::unordered_set<std::string>& builtInToolNames() {
    static const std::unordered_set<std::string> s_builtins = {
        "play",
        "playback",
        "volume",
        "music_settings",
        "notes",
        "set_led_strip",
        "ringing_alarm",
        "set_alarm",
        "set_timer",
        "set_reminder",
        "list_schedule",
        "cancel_scheduled",
        "acknowledge_reminders",
        "save_to_memory",
        "get_weather"
    };
    return s_builtins;
}

bool isValidToolName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;

    char first = name[0];
    if (!std::isalpha(static_cast<unsigned char>(first)) && first != '_') {
        return false;
    }

    for (size_t i = 1; i < name.size(); ++i) {
        char c = name[i];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != '-') {
            return false;
        }
    }
    return true;
}

bool isUrlAllowed(const std::string& url, std::string* error) {
    if (url.empty()) {
        if (error) *error = "URL cannot be empty";
        return false;
    }

    // 1. Check scheme
    bool is_https = (url.rfind("https://", 0) == 0);
    bool is_http = (url.rfind("http://", 0) == 0);

    if (!is_https && !is_http) {
        if (error) *error = "URL must use http:// or https:// scheme";
        return false;
    }

    // HTTPS is always permitted
    if (is_https) return true;

    // 2. HTTP is only permitted for local LAN / private addresses
    std::string host = extractHost(url, false);
    if (host.empty()) {
        if (error) *error = "URL is missing host component";
        return false;
    }

    if (host == "localhost") return true;

    if (host.size() > 6 && host.substr(host.size() - 6) == ".local") {
        return true;
    }

    int octets[4];
    if (parseIPv4(host, octets)) {
        if (isPrivateIPv4(octets)) {
            return true;
        }
        if (error) {
            *error = "Plain HTTP is only permitted for private LAN IPs (10.x, 192.168.x, 172.16-31.x, 127.x)";
        }
        return false;
    }

    if (error) {
        *error = "Plain HTTP is not allowed for public hostnames; use https:// or private LAN host";
    }
    return false;
}

std::string extractJsonRpcBody(std::string_view response, int expected_id) {
    // 1. Skip leading whitespace
    size_t start = 0;
    while (start < response.size() && std::isspace(static_cast<unsigned char>(response[start]))) {
        start++;
    }
    if (start >= response.size()) return "";

    std::string_view trimmed = response.substr(start);

    // 2. If it starts with '{' or '[', treat as application/json
    if (trimmed.front() == '{' || trimmed.front() == '[') {
        if (expected_id < 0) {
            return std::string(trimmed);
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, trimmed);
        if (!err) {
            if (doc.is<JsonObject>()) {
                if ((doc["id"] | -999999) == expected_id || expected_id < 0) {
                    return std::string(trimmed);
                }
            } else if (doc.is<JsonArray>()) {
                for (JsonObject item : doc.as<JsonArray>()) {
                    if ((item["id"] | -999999) == expected_id) {
                        std::string out;
                        serializeJson(item, out);
                        return out;
                    }
                }
            }
        }
        return std::string(trimmed);
    }

    // 3. Otherwise treat as text/event-stream; look for "data:" lines
    std::string matched_data;
    std::string last_data;

    std::istringstream stream((std::string(trimmed)));
    std::string line;
    while (std::getline(stream, line)) {
        // Strip trailing \r if any
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.rfind("data:", 0) != 0) continue;

        std::string_view content = line;
        content.remove_prefix(5); // drop "data:"
        while (!content.empty() && (content.front() == ' ' || content.front() == '\t')) {
            content.remove_prefix(1);
        }

        if (content.empty()) continue;

        last_data = std::string(content);

        if (expected_id >= 0) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, content);
            if (!err) {
                if (doc.is<JsonObject>() && (doc["id"] | -999999) == expected_id) {
                    matched_data = std::string(content);
                    break;
                } else if (doc.is<JsonArray>()) {
                    for (JsonObject item : doc.as<JsonArray>()) {
                        if ((item["id"] | -999999) == expected_id) {
                            std::string out;
                            serializeJson(item, out);
                            matched_data = out;
                            break;
                        }
                    }
                    if (!matched_data.empty()) break;
                }
            }
        }
    }

    if (!matched_data.empty()) return matched_data;
    return last_data;
}

ConvertToolsResult convertToolsListToDeclarations(
    std::string_view json_rpc_result,
    uint8_t max_tools,
    const std::unordered_set<std::string>& built_in_tools
) {
    ConvertToolsResult result;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json_rpc_result);
    if (err) {
        result.skipped.push_back(std::string("failed to parse tools/list JSON: ") + err.c_str());
        return result;
    }

    JsonArrayConst toolsArray;
    if (doc["tools"].is<JsonArrayConst>()) {
        toolsArray = doc["tools"].as<JsonArrayConst>();
    } else if (doc["result"]["tools"].is<JsonArrayConst>()) {
        toolsArray = doc["result"]["tools"].as<JsonArrayConst>();
    } else if (doc.is<JsonArrayConst>()) {
        toolsArray = doc.as<JsonArrayConst>();
    } else {
        result.skipped.push_back("no 'tools' array found in result");
        return result;
    }

    for (JsonObjectConst tool : toolsArray) {
        if (tool.isNull()) continue;

        const char* name_str = tool["name"] | "";
        std::string name(name_str);

        // 1. Validate tool name
        if (!isValidToolName(name)) {
            result.skipped.push_back("invalid name '" + name + "'");
            continue;
        }

        // 2. Check collisions with built-in tools
        if (built_in_tools.find(name) != built_in_tools.end()) {
            result.skipped.push_back("collides with built-in tool '" + name + "'");
            continue;
        }

        // 3. Enforce max_tools count limit
        if (result.tools.size() >= max_tools) {
            result.skipped.push_back("max tools limit reached (" + std::to_string(max_tools) + "): '" + name + "'");
            continue;
        }

        const char* desc_str = tool["description"] | "";
        std::string desc(desc_str);

        // Build Gemini declaration object:
        // { "name": "...", "description": "...", "parametersJsonSchema": <inputSchema> }
        JsonDocument declDoc;
        JsonObject decl = declDoc.to<JsonObject>();
        decl["name"] = name;
        decl["description"] = desc;

        if (tool.containsKey("inputSchema") && !tool["inputSchema"].isNull()) {
            decl["parametersJsonSchema"] = tool["inputSchema"];
        } else {
            decl["parametersJsonSchema"].to<JsonObject>()["type"] = "object";
        }

        std::string decl_json;
        serializeJson(declDoc, decl_json);

        // 4. Enforce ~24 KB total declaration payload limit
        if (result.total_declaration_bytes + decl_json.size() > MAX_DECLARATIONS_TOTAL_BYTES) {
            result.skipped.push_back("exceeded 24 KB declaration limit: '" + name + "'");
            continue;
        }

        result.total_declaration_bytes += decl_json.size();
        result.tools.push_back(ConvertedTool{
            std::move(name),
            std::move(desc),
            std::move(decl_json)
        });
    }

    return result;
}

std::string formatToolResponseForGemini(std::string_view json_rpc_result) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json_rpc_result);
    if (err) {
        return "{\"status\":\"error\",\"message\":\"Failed to parse tool result JSON\"}";
    }

    JsonObjectConst root = doc.is<JsonObjectConst>() ? doc.as<JsonObjectConst>() : JsonObjectConst();
    JsonObjectConst resObj = root;
    if (root.containsKey("result") && root["result"].is<JsonObjectConst>()) {
        resObj = root["result"].as<JsonObjectConst>();
    }

    // Check isError
    bool is_error = resObj["isError"] | false;
    if (is_error) {
        std::string err_msg;
        if (resObj.containsKey("content") && resObj["content"].is<JsonArrayConst>()) {
            for (JsonObjectConst item : resObj["content"].as<JsonArrayConst>()) {
                const char* text = item["text"] | "";
                if (text[0] != '\0') {
                    if (!err_msg.empty()) err_msg += " ";
                    err_msg += text;
                }
            }
        }
        if (err_msg.empty()) {
            err_msg = resObj["message"] | root["error"]["message"] | "Tool execution failed";
        }

        JsonDocument outDoc;
        outDoc["status"] = "error";
        outDoc["message"] = err_msg;
        std::string out;
        serializeJson(outDoc, out);
        return out;
    }

    // Join text contents
    std::string joined_text;
    if (resObj.containsKey("content") && resObj["content"].is<JsonArrayConst>()) {
        for (JsonObjectConst item : resObj["content"].as<JsonArrayConst>()) {
            const char* text = item["text"] | "";
            if (text[0] != '\0') {
                if (!joined_text.empty()) joined_text += "\n";
                joined_text += text;
            }
        }
    }

    JsonDocument outDoc;
    outDoc["result"] = joined_text;

    if (resObj.containsKey("structuredContent") && !resObj["structuredContent"].isNull()) {
        outDoc["structuredContent"] = resObj["structuredContent"];
    }

    std::string out;
    serializeJson(outDoc, out);
    return out;
}

} // namespace Mcp
