from agentcore import END, START, StateGraph


custom_tool_calls = []
custom_model_calls = []
arity_tool_calls = {"zero": 0, "one": 0}
arity_model_calls = {"one": 0, "two": 0}


async def custom_tool_handler(request, metadata):
    custom_tool_calls.append((request, dict(metadata)))
    assert metadata["name"] == "py_upper"
    assert metadata["decode_input"] == "json"
    return {
        "upper": str(request["text"]).upper(),
        "adapter_name": metadata["name"],
    }


async def custom_model_handler(prompt, schema, metadata):
    custom_model_calls.append((prompt, schema, dict(metadata)))
    assert metadata["name"] == "py_summarizer"
    return {
        "output": {
            "summary": f"{prompt['topic']}::{schema['style']}::{metadata['max_tokens']}",
        },
        "confidence": 0.75,
        "token_usage": len(str(prompt["topic"])),
    }


def zero_arg_tool_handler():
    arity_tool_calls["zero"] += 1
    return {"mode": "zero"}


def one_arg_tool_handler(request):
    arity_tool_calls["one"] += 1
    return {"echo": request["text"], "size": len(str(request["text"]))}


def one_arg_model_handler(prompt):
    arity_model_calls["one"] += 1
    return {"prompt": prompt["topic"]}


def two_arg_model_handler(prompt, schema):
    arity_model_calls["two"] += 1
    return {"summary": f"{prompt['topic']}::{schema['style']}"}


def adapter_node(state, config, runtime):
    topic = str(state.get("topic", "unset"))
    put_details = runtime.invoke_tool_with_metadata(
        "kv_store",
        f"action=put\ntable=memory\nkey=topic\nvalue={topic}",
        decode="text",
    )
    fetched_topic = runtime.invoke_tool(
        "kv_store",
        "action=get\ntable=memory\nkey=topic",
        decode="text",
    )
    model_details = runtime.invoke_model_with_metadata(
        "summarizer",
        f"topic::{fetched_topic}",
        max_tokens=14,
        decode="text",
    )
    custom_tool_details = runtime.invoke_tool_with_metadata(
        "py_upper",
        {"text": fetched_topic},
        decode="json",
    )
    custom_model_details = runtime.invoke_model_with_metadata(
        "py_summarizer",
        {"topic": custom_tool_details["output"]["upper"]},
        schema={"style": "brief"},
        max_tokens=11,
        decode="json",
    )
    return {
        "stored": put_details["output"],
        "fetched_topic": fetched_topic,
        "summary": model_details["output"],
        "tool_error_category": put_details["error_category"],
        "model_error_category": model_details["error_category"],
        "tool_attempts": put_details["attempts"],
        "model_token_usage": model_details["token_usage"],
        "custom_upper": custom_tool_details["output"]["upper"],
        "custom_adapter_name": custom_tool_details["output"]["adapter_name"],
        "custom_summary": custom_model_details["output"]["summary"],
        "custom_confidence": custom_model_details["confidence"],
        "custom_model_error_category": custom_model_details["error_category"],
        "custom_model_token_usage": custom_model_details["token_usage"],
    }


graph = StateGraph(dict, name="python_adapter_runtime_smoke", worker_count=2)
graph.add_node("adapter_node", adapter_node, kind="tool")
graph.add_edge(START, "adapter_node")
graph.add_edge("adapter_node", END)
compiled = graph.compile()

compiled.tools.register_sqlite("kv_store", policy={"retry_limit": 1})
compiled.models.register_local("summarizer", default_max_tokens=32)
compiled.models.register_grok_chat(
    "grok_builtin",
    transport={
        "base_url": "https://api.x.ai/v1",
        "bearer_token": "xai-test",
    },
    provider_model_name="grok-4",
    system_prompt="Route deeply.",
)
compiled.models.register_gemini_generate_content(
    "gemini_builtin",
    transport={
        "base_url": "https://generativelanguage.googleapis.com/v1beta",
        "api_key": "gemini-test",
        "api_key_header": "x-goog-api-key",
    },
    provider_model_name="gemini-2.5-flash",
    system_prompt="Be precise.",
)
compiled.tools.register(
    "py_upper",
    custom_tool_handler,
    decode_input="json",
    metadata={
        "display_name": "Python Upper Tool",
        "capabilities": [
            "sync",
            "async",
            "checkpoint_safe",
            "structured_request",
            "structured_response",
        ],
        "request_format": "json",
        "response_format": "json",
    },
)
compiled.tools.register("py_zero", zero_arg_tool_handler, decode_input="json")
compiled.tools.register("py_echo", one_arg_tool_handler, decode_input="json")
compiled.models.register(
    "py_summarizer",
    custom_model_handler,
    decode_prompt="json",
    decode_schema="json",
    metadata={
        "display_name": "Python Summarizer",
        "capabilities": [
            "sync",
            "async",
            "checkpoint_safe",
            "structured_request",
            "structured_response",
        ],
        "request_format": "json",
        "response_format": "json",
    },
)
compiled.models.register("py_prompt_only", one_arg_model_handler, decode_prompt="json")
compiled.models.register(
    "py_prompt_schema",
    two_arg_model_handler,
    decode_prompt="json",
    decode_schema="json",
)

tool_specs = {spec["name"]: spec for spec in compiled.tools.list()}
assert {"kv_store", "py_upper", "py_zero", "py_echo"} <= set(tool_specs)
tool_spec = compiled.tools.describe("kv_store")
assert tool_spec is not None
assert tool_spec["metadata"]["implementation"] == "sqlite_tool"
assert "sql" in tool_spec["metadata"]["capabilities"]
assert tool_spec["policy"]["retry_limit"] == 1

python_tool_spec = compiled.tools.describe("py_upper")
assert python_tool_spec is not None
assert python_tool_spec["metadata"]["provider"] == "python"
assert python_tool_spec["metadata"]["implementation"] == "python_callable_tool"
assert python_tool_spec["metadata"]["transport"] == "in_process"
assert python_tool_spec["metadata"]["request_format"] == "json"
assert "structured_request" in python_tool_spec["metadata"]["capabilities"]

model_specs = {spec["name"]: spec for spec in compiled.models.list()}
assert {
    "summarizer",
    "py_summarizer",
    "py_prompt_only",
    "py_prompt_schema",
    "grok_builtin",
    "gemini_builtin",
} <= set(model_specs)
model_spec = compiled.models.describe("summarizer")
assert model_spec is not None
assert model_spec["metadata"]["implementation"] == "local_model"
assert model_spec["metadata"]["transport"] == "in_process"

grok_spec = compiled.models.describe("grok_builtin")
assert grok_spec is not None
assert grok_spec["metadata"]["provider"] == "xai"
assert grok_spec["metadata"]["implementation"] == "grok_chat"
assert grok_spec["metadata"]["auth"] == "bearer_token"
assert "chat_messages" in grok_spec["metadata"]["capabilities"]

gemini_spec = compiled.models.describe("gemini_builtin")
assert gemini_spec is not None
assert gemini_spec["metadata"]["provider"] == "google_gemini"
assert gemini_spec["metadata"]["implementation"] == "gemini_generate_content"
assert gemini_spec["metadata"]["auth"] == "api_key"
assert "json_schema" in gemini_spec["metadata"]["capabilities"]

python_model_spec = compiled.models.describe("py_summarizer")
assert python_model_spec is not None
assert python_model_spec["metadata"]["provider"] == "python"
assert python_model_spec["metadata"]["implementation"] == "python_callable_model"
assert python_model_spec["metadata"]["response_format"] == "json"
assert "structured_response" in python_model_spec["metadata"]["capabilities"]

seed_result = compiled.tools.invoke("kv_store", "action=put\ntable=memory\nkey=seed\nvalue=ready", decode="text")
assert seed_result == "ok"
seed_value = compiled.tools.invoke("kv_store", "action=get\ntable=memory\nkey=seed", decode="text")
assert seed_value == "ready"

model_probe = compiled.models.invoke("summarizer", "adapter-surface", max_tokens=7, decode="text")
assert model_probe == "local::adapter"

python_tool_probe = compiled.tools.invoke("py_upper", {"text": "native-adapters"}, decode="json")
assert python_tool_probe == {
    "upper": "NATIVE-ADAPTERS",
    "adapter_name": "py_upper",
}
assert compiled.tools.invoke("py_zero", {"ignored": True}, decode="json") == {"mode": "zero"}
assert compiled.tools.invoke("py_echo", {"text": "native-adapters"}, decode="json") == {
    "echo": "native-adapters",
    "size": len("native-adapters"),
}
python_model_probe_details = compiled.models.invoke_with_metadata(
    "py_summarizer",
    {"topic": "NATIVE-ADAPTERS"},
    schema={"style": "brief"},
    max_tokens=5,
    decode="json",
)
assert python_model_probe_details["output"] == {
    "summary": "NATIVE-ADAPTERS::brief::5",
}
assert python_model_probe_details["confidence"] == 0.75
assert python_model_probe_details["token_usage"] == len("NATIVE-ADAPTERS")
assert compiled.models.invoke("py_prompt_only", {"topic": "NATIVE-ADAPTERS"}, decode="json") == {
    "prompt": "NATIVE-ADAPTERS",
}
assert compiled.models.invoke(
    "py_prompt_schema",
    {"topic": "NATIVE-ADAPTERS"},
    schema={"style": "brief"},
    decode="json",
) == {
    "summary": "NATIVE-ADAPTERS::brief",
}

final_state = compiled.invoke({"topic": "native-adapters"})
assert final_state["stored"] == "ok"
assert final_state["fetched_topic"] == "native-adapters"
assert final_state["summary"] == "local::" + "topic::native-adapters"[:14]
assert final_state["tool_error_category"] == "none"
assert final_state["model_error_category"] == "none"
assert final_state["tool_attempts"] == 1
assert final_state["model_token_usage"] == 14
assert final_state["custom_upper"] == "NATIVE-ADAPTERS"
assert final_state["custom_adapter_name"] == "py_upper"
assert final_state["custom_summary"] == "NATIVE-ADAPTERS::brief::11"
assert final_state["custom_confidence"] == 0.75
assert final_state["custom_model_error_category"] == "none"
assert final_state["custom_model_token_usage"] == len("NATIVE-ADAPTERS")

assert len(custom_tool_calls) >= 2
assert len(custom_model_calls) >= 2
assert arity_tool_calls == {"zero": 1, "one": 1}
assert arity_model_calls == {"one": 1, "two": 1}
assert custom_tool_calls[0][0] == {"text": "native-adapters"}
assert custom_tool_calls[0][1]["name"] == "py_upper"
assert custom_model_calls[0][1] == {"style": "brief"}

print("python adapters runtime smoke passed")
