"""tools::Registry: routing, answers now and later, remote (MCP) tools,
the start/end events and the busy flag that holds a session open."""
import json

from waveshare_host import ToolHarness, tools_set_now_ms, tools_status_of

OTHER, SEARCH, MUSIC, BLUETOOTH, INFO, DEVICE = range(6)


def test_schema_tool_answers_now_with_decoded_args():
    tools_set_now_ms(1000)
    h = ToolHarness()
    assert h.add("play", MUSIC, '{"status":"success"}')
    assert h.dispatch("c1", "play", '{"query":"blinding lights"}')
    decoded, args = h.seen_args("play")
    assert decoded and json.loads(args) == {"query": "blinding lights"}
    assert h.responses == [("c1", '{"status":"success"}')]
    assert [e[:4] for e in h.events] == [("started", "play", "music", False), ("ended", "play", "music", False)]
    assert h.events[1][4] == "success"
    assert not h.busy(60000)


def test_missing_required_argument_is_rejected_without_calling_the_tool():
    h = ToolHarness()
    h.add("play", MUSIC, '{"status":"success"}')
    assert not h.dispatch("c1", "play", "{}")
    assert h.seen_args("play") == (False, "")
    (call_id, answer), = h.responses
    assert call_id == "c1" and json.loads(answer)["status"] == "error"
    assert "query" in json.loads(answer)["message"]
    assert h.events == []


def test_unknown_tool_is_answered_with_an_error():
    h = ToolHarness()
    assert not h.dispatch("c9", "no_such_tool")
    assert json.loads(h.responses[0][1])["status"] == "error"


def test_later_answer_keeps_the_registry_busy_until_answered():
    tools_set_now_ms(5000)
    h = ToolHarness()
    h.add("get_weather", INFO)   # answers later
    assert h.dispatch("w1", "get_weather", '{"location":"Pune"}')
    assert h.responses == []
    assert h.busy(60000)
    tools_set_now_ms(7500)
    h.answer("w1", '{"status":"success","temp":21}')
    assert not h.busy(60000)
    assert h.responses == [("w1", '{"status":"success","temp":21}')]
    ended = h.events[-1]
    assert ended[0] == "ended" and ended[1] == "get_weather" and ended[5] == 2500


def test_busy_gives_up_on_a_tool_that_never_answers():
    tools_set_now_ms(0)
    h = ToolHarness()
    h.add("get_weather", INFO)
    h.dispatch("w1", "get_weather")
    tools_set_now_ms(59000)
    assert h.busy(60000)
    tools_set_now_ms(61000)
    assert not h.busy(60000)


def test_remote_tools_route_and_report_their_kind():
    tools_set_now_ms(0)
    h = ToolHarness()
    h.add("play", MUSIC, '{"status":"success"}')
    h.set_remote(["web_search", "news"])
    assert h.dispatch("r1", "web_search", '{"query":"f1"}')
    assert h.remote_calls == ["web_search"]
    assert h.events[0][:4] == ("started", "web_search", "search", True)
    assert h.busy(60000)
    h.answer("r1", '{"status":"not_found"}')
    assert h.events[-1][4] == "not_found"
    assert not h.busy(60000)
    # A name the server doesn't have: an error, and the call isn't left running.
    assert not h.dispatch("r2", "missing")
    assert json.loads(h.responses[-1][1])["status"] == "error"
    assert not h.busy(60000)


def test_declarations_only_for_registered_tools_then_remote():
    h = ToolHarness()
    h.add("volume", MUSIC, '{"status":"success"}')
    h.add("play", MUSIC, '{"status":"success"}')
    h.add("ping", OTHER, '{"status":"success"}', '{"name":"ping","description":"Answers pong."}')
    h.set_remote(["web_search"])
    # Schema order for schema tools, then own declarations, then remote.
    assert h.declared() == ["play", "volume", "ping", "web_search"]
    assert h.declared(skip="web_search") == ["play", "volume", "ping"]
    assert h.declared(remote=False) == ["play", "volume", "ping"]


def test_tool_with_own_declaration_gets_raw_args():
    h = ToolHarness()
    h.add("ping", OTHER, '{"status":"success"}', '{"name":"ping"}')
    assert h.dispatch("p1", "ping", '{"x":1}')
    assert h.seen_args("ping") == (False, '{"x":1}')


def test_duplicate_names_are_refused():
    h = ToolHarness()
    assert h.add("play", MUSIC, '{"status":"success"}')
    assert not h.add("play", MUSIC, '{"status":"success"}')


def test_status_of_answers():
    assert tools_status_of('{"status":"success"}') == "success"
    assert tools_status_of('{"temp":21}') == "success"
    assert tools_status_of('{"status":"ambiguous"}') == "ambiguous"
    assert tools_status_of('{"status":"not_found"}') == "not_found"
    assert tools_status_of('{"status":"error","message":"x"}') == "error"
    assert tools_status_of('{"error":"boom"}') == "error"
    assert tools_status_of("not json") == "error"
