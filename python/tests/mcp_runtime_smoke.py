from __future__ import annotations

import os
import sys

from agentcore import END, START, StateGraph
from agentcore.mcp import StdioMCPClient


FIXTURE = os.path.join(
    os.path.dirname(__file__),
    "fixtures",
    "mcp_stdio_server.py",
)


def launch_command() -> list[str]:
    return [sys.executable, FIXTURE]


captured_logs: list[dict[str, object]] = []
captured_notifications: list[dict[str, object]] = []


def sampling_handler(request, client):
    text_blocks: list[str] = []
    for message in request["messages"]:
        content = message["content"]
        if content["type"] == "text":
            text_blocks.append(str(content.get("text", "")))
    return {
        "role": "assistant",
        "content": {
            "type": "text",
            "text": "sample::" + " | ".join(text_blocks),
        },
        "model": "fixture-sampler",
        "stopReason": "endTurn",
    }


def elicitation_handler(request, client):
    return {
        "action": "accept",
        "content": {
            "reviewer": "fixture-reviewer",
            "approved": True,
        },
    }


with StdioMCPClient(
    launch_command(),
    startup_timeout=10.0,
    request_timeout=10.0,
    roots=[
        "file:///workspace/agentcore",
        "file:///workspace/notes",
    ],
    sampling_handler=sampling_handler,
    elicitation_handler=elicitation_handler,
    log_handler=lambda entry, owner: captured_logs.append(dict(entry.items())),
    notification_handler=lambda message, owner: captured_notifications.append(dict(message.items())),
) as client:
    assert client.server_info["name"] == "agentcore-fixture"
    assert client.server_capabilities["logging"] == {}
    assert client.server_capabilities["resources"]["subscribe"] is True
    assert client.server_capabilities["tools"]["listChanged"] is True
    tools = client.list_tools()
    tool_names = sorted(tool["name"] for tool in tools)
    assert tool_names == [
        "fail_tool",
        "optional_context",
        "register_dynamic_assets",
        "sum_numbers",
        "upper",
    ]
    assert client.call_tool("upper", {"text": "hello"}) == {"upper": "HELLO"}
    assert client.call_tool("sum_numbers", {"values": [2, 3, 5]}) == {"total": 10}
    raw_failure = client.call_tool("fail_tool", {"value": "demo"}, result_mode="raw")
    assert raw_failure["isError"] is True
    assert raw_failure["structuredContent"]["output"]["reason"] == "bad::demo"

    prompts = client.list_prompts()
    prompt_names = sorted(prompt["name"] for prompt in prompts)
    assert prompt_names == ["investigate_topic", "review_code"]
    assert client.complete_prompt_argument("review_code", "language", "p") == ["python", "cpp", "rust"]

    review_prompt = client.get_prompt(
        "review_code",
        {
            "language": "python",
            "repository": "agentcore",
            "question": "How should we wire MCP?",
        },
    )
    assert "SYSTEM:" in review_prompt.to_text()
    assert "reviewing python code" in review_prompt.to_text().lower()
    assert review_prompt.to_model_input(mode="messages")[0]["content"]["type"] == "text"

    investigate_prompt = client.get_prompt(
        "investigate_topic",
        {"topic": "scheduler", "question": "What should we inspect?"},
    )
    investigate_text = investigate_prompt.to_text()
    assert "[RESOURCE uri=memo://guide/scheduler" in investigate_text
    assert "stay deterministic and explicit" in investigate_text

    resources = client.list_resources()
    assert [resource["uri"] for resource in resources] == ["memo://guide/overview"]
    resource_templates = client.list_resource_templates()
    assert [template["uriTemplate"] for template in resource_templates] == ["memo://guide/{topic}"]
    assert client.complete_resource_argument("memo://guide/{topic}", "topic", "s") == [
        "python",
        "cpp",
        "scheduler",
    ]
    overview = client.read_resource("memo://guide/overview")
    assert overview["project"] == "agentcore"
    scheduler_guide = client.read_resource("memo://guide/scheduler", decode="text")
    assert "Guide for scheduler" in scheduler_guide

    client.set_logging_level("warning")
    client.subscribe_resource("memo://guide/overview")
    optional_context = client.call_tool("optional_context", {"topic": "scheduler"})
    assert optional_context["root_count"] == 2
    assert optional_context["first_root"] == "file:///workspace/agentcore"
    assert optional_context["sampled_text"] == "sample::Summarize scheduler with 2 roots."
    assert optional_context["elicitation"]["action"] == "accept"
    assert optional_context["elicitation"]["content"]["reviewer"] == "fixture-reviewer"
    assert optional_context["logging_level"] == "warning"
    assert optional_context["resource_update_notified"] is True
    assert "memo://guide/overview" in optional_context["subscriptions"]

    drained_logs = client.drain_logs()
    assert len(drained_logs) == 1
    assert drained_logs[0]["level"] == "warning"
    assert drained_logs[0]["data"]["topic"] == "scheduler"
    assert captured_logs[-1]["level"] == "warning"
    assert captured_logs[-1]["data"]["root_count"] == 2

    drained_updates = client.drain_resource_updates()
    assert drained_updates == ["memo://guide/overview"]

    client.set_roots(["file:///workspace/new-root"])
    optional_context_after_roots = client.call_tool("optional_context", {"topic": "python"})
    assert optional_context_after_roots["root_count"] == 1
    assert optional_context_after_roots["first_root"] == "file:///workspace/new-root"
    assert optional_context_after_roots["roots_changed_count"] >= 1

    dynamic_assets = client.call_tool("register_dynamic_assets", {})
    assert dynamic_assets["tool_count"] >= 6
    assert dynamic_assets["prompt_count"] >= 3
    assert dynamic_assets["resource_count"] >= 2
    assert client.list_change_counts["tools"] >= 1
    assert client.list_change_counts["prompts"] >= 1
    assert client.list_change_counts["resources"] >= 1
    assert "dynamic_echo" in [tool["name"] for tool in client.list_tools()]
    assert "dynamic_prompt" in [prompt["name"] for prompt in client.list_prompts()]
    assert "memo://guide/dynamic" in [resource["uri"] for resource in client.list_resources()]
    assert client.call_tool("dynamic_echo", {"value": "ok"}) == {"echo": "ok"}
    assert any(
        notification["method"] == "notifications/resources/updated"
        for notification in captured_notifications
    )


def mcp_node(state, config, runtime):
    remote_upper = runtime.invoke_tool(
        "remote_upper",
        {"text": state["text"]},
        decode="json",
    )
    remote_total = runtime.invoke_tool(
        "remote_sum_numbers",
        {"values": state["values"]},
        decode="json",
    )
    remote_failure = runtime.invoke_tool(
        "remote_fail_tool",
        {"value": state["text"]},
        decode="json",
    )
    return {
        "upper": remote_upper["upper"],
        "total": remote_total["total"],
        "failure_reason": remote_failure["structuredContent"]["output"]["reason"],
        "failure_is_error": remote_failure["isError"],
    }


graph = StateGraph(dict, name="mcp_runtime_smoke", worker_count=2)
graph.add_node("mcp_node", mcp_node, kind="tool")
graph.add_edge(START, "mcp_node")
graph.add_edge("mcp_node", END)
compiled = graph.compile()

compiled.models.register(
    "echo_prompt_text",
    lambda prompt: {"prompt": prompt},
    decode_prompt="text",
)
compiled.models.register(
    "echo_prompt_messages",
    lambda prompt: {"prompt": prompt},
    decode_prompt="json",
)

client = compiled.tools.register_mcp_stdio(
    launch_command(),
    prefix="remote",
    request_timeout=10.0,
    tool_timeout=10.0,
)
registered = sorted(tool["name"] for tool in compiled.tools.list() if tool["metadata"]["provider"] == "mcp")
assert registered == [
    "remote_fail_tool",
    "remote_optional_context",
    "remote_register_dynamic_assets",
    "remote_sum_numbers",
    "remote_upper",
]
assert client.server_info["name"] == "agentcore-fixture"

result = compiled.invoke({"text": "bridge", "values": [1, 4, 7]})
assert result["upper"] == "BRIDGE"
assert result["total"] == 12
assert result["failure_reason"] == "bad::bridge"
assert result["failure_is_error"] is True

prompt_for_model = client.get_prompt(
    "investigate_topic",
    {"topic": "cpp", "question": "How should we profile it?"},
)
prompt_model_result = compiled.models.invoke(
    "echo_prompt_text",
    prompt_for_model,
    decode="json",
)
assert "[RESOURCE uri=memo://guide/cpp" in prompt_model_result["prompt"]

prompt_messages_result = compiled.models.invoke(
    "echo_prompt_messages",
    prompt_for_model.to_model_input(mode="messages"),
    decode="json",
)
assert prompt_messages_result["prompt"][1]["content"]["type"] == "resource"
