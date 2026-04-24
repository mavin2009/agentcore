# MCP Integration

AgentCore now includes a fuller Model Context Protocol surface over `stdio`.

The implemented scope is:

- MCP client handshake over `stdio`
- `tools/list` and `tools/call`
- `prompts/list` and `prompts/get`
- `resources/list`, `resources/templates/list`, and `resources/read`
- `resources/subscribe` and `resources/unsubscribe`
- `completion/complete`
- `logging/setLevel` plus `notifications/message`
- `roots/list` plus `notifications/roots/list_changed`
- `sampling/createMessage`
- `elicitation/create`
- tool, prompt, and resource list-changed notifications
- resource-update notifications
- exposing AgentCore tools through an MCP server surface
- exposing prompts and resources through the same server surface
- packaged launcher/config helpers for Claude, Codex, and Gemini

## Installed Launcher

The published Python package now installs:

- `agentcore-mcp`
- `agentcore-mcp-server`
- `agentcore-mcp-config`

The serving path is intentionally small: point the launcher at a module reference or a Python file, and it will load an `MCPServer`, a server factory, a compiled graph, a graph builder, or a tool registry.

Examples:

```bash
agentcore-mcp-server --target ./my_server.py:build_server
python -m agentcore.mcp serve --target package.module:create_server
```

Accepted target forms:

- `package.module:server`
- `package.module:build_server`
- `./local_server.py:server`
- `./local_server.py:create_server`

If the target resolves to a graph or tool registry instead of an `MCPServer`, AgentCore wraps it through `MCPServer.from_compiled_graph(...)` or `MCPServer.from_tool_registry(...)`.

## Why This Shape

AgentCore already had:

- graph-owned tool registries
- reusable prompt objects
- native runtime tool/model invocation
- a small Python surface for adapter registration and prompt rendering

That makes MCP a good interoperability layer rather than a new orchestration layer. The graph runtime still owns execution, state patches, checkpoints, traces, and replay semantics. MCP is used to move tools, prompts, and contextual resources across process boundaries without changing the execution model.

## Connect To An External MCP Tool Server

Use `compiled.tools.register_mcp_stdio(...)` when the main need is to import remote MCP tools into a compiled graph's ordinary tool registry.

```python
import sys

from agentcore.graph import END, START, StateGraph


def use_remote_tool(state, config, runtime):
    result = runtime.invoke_tool(
        "remote_upper",
        {"text": state["text"]},
        decode="json",
    )
    return {"upper": result["upper"]}


graph = StateGraph(dict, name="mcp_client_demo", worker_count=2)
graph.add_node("use_remote_tool", use_remote_tool, kind="tool")
graph.add_edge(START, "use_remote_tool")
graph.add_edge("use_remote_tool", END)
compiled = graph.compile()

compiled.tools.register_mcp_stdio(
    [sys.executable, "./python/tests/fixtures/mcp_stdio_server.py"],
    prefix="remote",
)

result = compiled.invoke({"text": "hello"})
```

Imported tools are ordinary AgentCore tools after registration, so graph nodes still call them through `runtime.invoke_tool(...)`.

Supported bridge options:

- `prefix=` namespaces imported tool names locally
- `include=` and `exclude=` filter which remote tools are mirrored
- `env=` and `cwd=` control child process launch context
- `startup_timeout=` and `request_timeout=` control MCP handshake/request timing
- `tool_timeout=` controls per-call timeout for mirrored tool invocations
- `result_mode="auto" | "raw"` controls whether successful tool results are normalized or returned as raw MCP envelopes
- `argument_key=` wraps non-mapping requests into `{argument_key: request}`

## Use The MCP Client Directly

If you want the protocol surface directly instead of registry mirroring, use `agentcore.mcp.StdioMCPClient`.

```python
import sys

from agentcore.mcp import StdioMCPClient


with StdioMCPClient([sys.executable, "./python/tests/fixtures/mcp_stdio_server.py"]) as client:
    tools = client.list_tools()
    prompts = client.list_prompts()
    resources = client.list_resources()

    tool_result = client.call_tool("upper", {"text": "hello"})
    prompt = client.get_prompt(
        "review_code",
        {
            "language": "python",
            "repository": "agentcore",
            "question": "How should we wire MCP?",
        },
    )
    guide = client.read_resource("memo://guide/overview")
```

Current client surface:

- `start()`
- `list_tools()`
- `call_tool(...)`
- `call_tool_raw(...)`
- `list_prompts()`
- `get_prompt(...)`
- `get_prompt_raw(...)`
- `list_resources()`
- `list_resource_templates()`
- `read_resource(...)`
- `read_resource_raw(...)`
- `complete(...)`
- `complete_prompt_argument(...)`
- `complete_resource_argument(...)`
- `list_roots()`
- `set_roots(...)`
- `set_sampling_handler(...)`
- `set_elicitation_handler(...)`
- `set_log_handler(...)`
- `set_notification_handler(...)`
- `subscribe_resource(...)`
- `unsubscribe_resource(...)`
- `set_logging_level(...)`
- `drain_notifications()`
- `drain_logs()`
- `drain_resource_updates()`
- `close()`

`get_prompt(...)` returns `RenderedMCPPrompt`, which can be flattened to text or passed through the ordinary model-input path:

```python
rendered = client.get_prompt("investigate_topic", {"topic": "scheduler", "question": "What should we inspect?"})
text_payload = rendered.to_model_input()
message_payload = rendered.to_model_input(mode="messages")
```

`RenderedMCPPrompt.to_text()` inlines resource-text blocks and preserves simple markers for non-text content, which makes it usable with text-oriented model adapters while still exposing the richer message form when needed.

Optional client capabilities are configured up front when you construct `StdioMCPClient`, which keeps the surface simple and matches MCP capability negotiation:

```python
from agentcore.mcp import StdioMCPClient


def sampling_handler(request, client):
    last_text = ""
    for message in request["messages"]:
        content = message["content"]
        if content["type"] == "text":
            last_text = str(content.get("text", ""))
    return {
        "role": "assistant",
        "content": {"type": "text", "text": f"sample::{last_text}"},
        "model": "fixture-sampler",
        "stopReason": "endTurn",
    }


def elicitation_handler(request, client):
    return {
        "action": "accept",
        "content": {"reviewer": "agentcore", "approved": True},
    }


with StdioMCPClient(
    [sys.executable, "./python/tests/fixtures/mcp_stdio_server.py"],
    roots=["file:///workspace/agentcore"],
    sampling_handler=sampling_handler,
    elicitation_handler=elicitation_handler,
) as client:
    client.set_logging_level("warning")
    client.subscribe_resource("memo://guide/overview")
```

## Configure Claude, Codex, And Gemini

Once `agentcore-graph` is installed, the config helper can render ready-to-paste client configuration using the current Python interpreter path:

```bash
agentcore-mcp-config claude --name local-agentcore --target ./my_server.py:build_server
agentcore-mcp-config codex --name local-agentcore --target ./my_server.py:build_server
agentcore-mcp-config gemini --name local-agentcore --target ./my_server.py:build_server
```

That command prints:

- Claude Code `claude mcp add-json ...` plus a `.mcp.json` snippet
- Claude Desktop `claude_desktop_config.json`-style JSON
- Codex `codex mcp add ...` plus a `~/.codex/config.toml` snippet
- Gemini CLI `gemini mcp add ...` plus a `settings.json` snippet

The generated command path uses:

```bash
python -m agentcore.mcp serve --target ...
```

instead of assuming a globally visible console script, which makes the output safer for virtual environments and packaged Python installs.

## Expose Tools, Prompts, And Resources Through MCP

Use `MCPServer` when you want AgentCore to act as an MCP server.

```python
from agentcore import ChatPromptTemplate
from agentcore.mcp import MCPServer


server = MCPServer(name="agentcore-fixture", version="0.1")


@server.tool("upper")
def upper(arguments):
    return {"upper": str(arguments["text"]).upper()}


server.add_prompt_template(
    "review_code",
    ChatPromptTemplate.from_messages(
        [
            ("system", "You are reviewing {language} code."),
            ("user", "Question: {question}"),
        ]
    ),
    arguments=[
        {"name": "language", "required": True},
        {"name": "question", "required": True},
    ],
)


server.add_resource(
    "memo://guide/overview",
    {"text": '{"project":"agentcore"}', "mimeType": "application/json"},
)


@server.resource_template("memo://guide/{topic}")
def guide(arguments):
    return {"text": f"Guide for {arguments['topic']}"}


server.run_stdio()
```

The server can also reuse graph-owned tool registries directly:

```python
from agentcore.mcp import MCPServer


server = MCPServer.from_compiled_graph(compiled, name="agentcore-tools")
server.run_stdio()
```

That path intentionally does not invent a separate MCP-only tool table. External callers hit the same graph-owned tool registry that graph nodes use through `runtime.invoke_tool(...)`.

When a handler accepts MCP metadata, AgentCore now injects a session object under both `metadata["session"]` and `metadata["mcp"]`. That session gives handlers comfortable access to the optional client-facing MCP features:

```python
@server.tool("optional_context")
def optional_context(arguments, metadata):
    session = metadata["mcp"]
    roots = session.list_roots()
    sampled = session.sample(
        [{"role": "user", "content": {"type": "text", "text": "Summarize this repo."}}],
        system_prompt="Keep it brief.",
        max_tokens=64,
    )
    elicited = session.elicit(
        "Provide reviewer context.",
        {
            "type": "object",
            "properties": {"reviewer": {"type": "string"}},
            "required": ["reviewer"],
            "additionalProperties": False,
        },
    )
    session.send_log("warning", {"root_count": len(roots)}, logger="agentcore.mcp")
    return {
        "roots": roots,
        "sampled": sampled,
        "elicited": elicited,
    }
```

## Prompt And Completion Semantics

Prompt handlers may return:

- a plain string
- a prompt object such as `PromptTemplate`, `ChatPromptTemplate`, `RenderedPrompt`, `RenderedChatPrompt`, or `RenderedMCPPrompt`
- a raw MCP-style prompt result mapping with `messages`
- a sequence of prompt messages

Prompt arguments are declared explicitly through `arguments=[...]`.

Prompt completions and resource-template completions are exposed through `completion/complete`. On the AgentCore side, you attach them with `argument_completions={...}` where each value can be:

- a static sequence of strings
- a callable returning a sequence or completion payload

## Resource Semantics

Static resources are exposed through `add_resource(...)`.

Parameterized resources are exposed through `add_resource_template(...)`. AgentCore currently supports simple `{name}` URI-template variables and matches them deterministically for `resources/read`.

Resource reads normalize to either:

- decoded Python objects for JSON text resources in `decode="auto"` mode
- text
- bytes
- raw resource-entry lists

## Validation

The end-to-end smoke coverage is:

```bash
PYTHONPATH=./build/python python3 ./python/tests/mcp_runtime_smoke.py
PYTHONPATH=./build/python python3 ./python/tests/mcp_launcher_smoke.py
```

That smoke test:

- launches an MCP stdio child process
- validates direct client handshake and capability discovery
- validates tool discovery and tool calls
- validates prompt discovery, prompt rendering, and completion calls
- validates resource discovery, template discovery, and resource reads
- validates resource subscriptions and resource-update notifications
- validates roots access and roots-change tracking
- validates server-initiated sampling and elicitation requests
- validates logging level control and log notifications
- validates tool/prompt/resource list-changed notifications after dynamic catalog updates
- validates `RenderedMCPPrompt` conversion into model input
- mirrors remote MCP tools into `compiled.tools`
- invokes those mirrored tools from inside a graph node
- validates the packaged launcher path using `python -m agentcore.mcp serve --target ...`
- validates generated Claude/Codex/Gemini configuration output
