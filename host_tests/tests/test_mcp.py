import json
import pytest
from waveshare_host import (
    mcp_is_valid_tool_name,
    mcp_is_url_allowed,
    mcp_extract_json_rpc_body,
    mcp_convert_tools_list,
    mcp_format_tool_response,
)


def test_tool_name_validation():
    # Valid names: ^[A-Za-z_][A-Za-z0-9_.-]{0,63}$
    assert mcp_is_valid_tool_name("news_headlines")
    assert mcp_is_valid_tool_name("weather")
    assert mcp_is_valid_tool_name("web_search")
    assert mcp_is_valid_tool_name("_custom_tool")
    assert mcp_is_valid_tool_name("tool.v1-beta")
    assert mcp_is_valid_tool_name("a" * 64)

    # Invalid names
    assert not mcp_is_valid_tool_name("")
    assert not mcp_is_valid_tool_name("1invalid_leading_digit")
    assert not mcp_is_valid_tool_name("-invalid_leading_dash")
    assert not mcp_is_valid_tool_name(".invalid_leading_dot")
    assert not mcp_is_valid_tool_name("tool with spaces")
    assert not mcp_is_valid_tool_name("tool@special")
    assert not mcp_is_valid_tool_name("a" * 65)


def test_url_security_policy():
    # HTTPS is always allowed
    ok, _ = mcp_is_url_allowed("https://mcp.your-domain.example/mcp")
    assert ok
    ok, _ = mcp_is_url_allowed("https://1.2.3.4:8443/mcp")
    assert ok

    # Plain HTTP allowed ONLY for local LAN / private IP ranges
    assert mcp_is_url_allowed("http://192.168.1.50:8788/mcp")[0]
    assert mcp_is_url_allowed("http://10.0.0.1:8787/mcp")[0]
    assert mcp_is_url_allowed("http://172.16.0.5:8080/mcp")[0]
    assert mcp_is_url_allowed("http://172.31.255.254:8787/mcp")[0]
    assert mcp_is_url_allowed("http://127.0.0.1:8787/mcp")[0]
    assert mcp_is_url_allowed("http://localhost:8787/mcp")[0]
    assert mcp_is_url_allowed("http://nexus.local:8787/mcp")[0]
    assert mcp_is_url_allowed("http://my-pc.local/mcp")[0]

    # Plain HTTP disallowed for public IPs and public domains
    ok, err = mcp_is_url_allowed("http://api.example.com/mcp")
    assert not ok
    assert "Plain HTTP" in err

    ok, err = mcp_is_url_allowed("http://8.8.8.8:8787/mcp")
    assert not ok

    ok, err = mcp_is_url_allowed("http://172.32.0.1/mcp")
    assert not ok  # outside 172.16.0.0/12

    # Invalid schemes
    ok, err = mcp_is_url_allowed("ftp://192.168.1.1/mcp")
    assert not ok
    ok, err = mcp_is_url_allowed("")
    assert not ok


def test_json_rpc_body_extraction():
    # 1. Plain application/json
    json_body = '{"jsonrpc":"2.0","id":1,"result":{"tools":[]}}'
    extracted = mcp_extract_json_rpc_body(json_body, 1)
    assert extracted == json_body

    # 2. Batch JSON array
    batch = '[{"jsonrpc":"2.0","id":1,"result":"first"},{"jsonrpc":"2.0","id":2,"result":"second"}]'
    extracted_batch = mcp_extract_json_rpc_body(batch, 2)
    parsed = json.loads(extracted_batch)
    assert parsed["id"] == 2
    assert parsed["result"] == "second"

    # 3. text/event-stream format with data: lines
    sse_body = (
        ": keep-alive\n"
        "event: message\n"
        'data: {"jsonrpc":"2.0","id":1,"result":"old"}\n\n'
        "event: message\n"
        'data: {"jsonrpc":"2.0","id":42,"result":{"status":"ok"}}\n\n'
    )
    extracted_sse = mcp_extract_json_rpc_body(sse_body, 42)
    parsed_sse = json.loads(extracted_sse)
    assert parsed_sse["id"] == 42
    assert parsed_sse["result"]["status"] == "ok"


def test_convert_tools_list_to_declarations():
    raw_tools = {
        "tools": [
            {
                "name": "custom_search",
                "description": "Performs custom search",
                "inputSchema": {
                    "type": "object",
                    "properties": {"query": {"type": "string"}},
                    "required": ["query"],
                },
            },
            {
                "name": "get_weather",  # Collides with built-in tool!
                "description": "Colliding weather tool",
                "inputSchema": {"type": "object"},
            },
            {
                "name": "9bad_tool_name",  # Invalid name!
                "description": "Bad name",
                "inputSchema": {"type": "object"},
            },
            {
                "name": "valid_extra",
                "description": "Valid extra tool",
                "inputSchema": {"type": "object"},
            },
        ]
    }
    raw_json = json.dumps(raw_tools)

    # Test conversion with max_tools=1
    tools, skipped, total_bytes = mcp_convert_tools_list(raw_json, 1)
    assert len(tools) == 1
    assert tools[0][0] == "custom_search"
    assert tools[0][1] == "Performs custom search"

    decl = json.loads(tools[0][2])
    assert decl["name"] == "custom_search"
    assert decl["parametersJsonSchema"]["required"] == ["query"]

    # Check skipped reasons
    assert any("collides with built-in" in s for s in skipped)
    assert any("invalid name" in s for s in skipped)
    assert any("max tools limit reached" in s for s in skipped)
    assert total_bytes > 0


def test_format_tool_response_for_gemini():
    # 1. Success with text content only
    resp1 = {
        "jsonrpc": "2.0",
        "id": 1,
        "result": {
            "content": [
                {"type": "text", "text": "Headline 1"},
                {"type": "text", "text": "Headline 2"},
            ]
        },
    }
    formatted1 = json.loads(mcp_format_tool_response(json.dumps(resp1)))
    assert formatted1["result"] == "Headline 1\nHeadline 2"
    assert "structuredContent" not in formatted1

    # 2. Success with structuredContent
    resp2 = {
        "jsonrpc": "2.0",
        "id": 2,
        "result": {
            "content": [{"type": "text", "text": "Current temp is 20 C"}],
            "structuredContent": {"temp": 20, "condition": "Cloudy"},
        },
    }
    formatted2 = json.loads(mcp_format_tool_response(json.dumps(resp2)))
    assert formatted2["result"] == "Current temp is 20 C"
    assert formatted2["structuredContent"]["temp"] == 20

    # 3. Tool error (isError = true)
    resp3 = {
        "jsonrpc": "2.0",
        "id": 3,
        "result": {
            "isError": True,
            "content": [{"type": "text", "text": "Location not found"}],
        },
    }
    formatted3 = json.loads(mcp_format_tool_response(json.dumps(resp3)))
    assert formatted3["status"] == "error"
    assert "Location not found" in formatted3["message"]
