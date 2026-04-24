from __future__ import annotations

from agentcore import END, START, StateGraph
from agentcore.mcp import MCPServer


def identity(state, config):
    return {}


graph = StateGraph(dict, name="installable_fixture_graph", worker_count=1)
graph.add_node("identity", identity)
graph.add_edge(START, "identity")
graph.add_edge("identity", END)
compiled = graph.compile()


compiled.tools.register(
    "echo_text",
    lambda request: {"echo": str(request.get("text", ""))},
    decode_input="json",
)


def build_server() -> MCPServer:
    server = MCPServer.from_compiled_graph(
        compiled,
        name="agentcore-installable-fixture",
        version="0.1",
        descriptions={
            "echo_text": "Echo text through the installable AgentCore MCP launcher.",
        },
        input_schemas={
            "echo_text": {
                "type": "object",
                "properties": {
                    "text": {"type": "string"},
                },
                "required": ["text"],
                "additionalProperties": False,
            }
        },
    )
    return server
