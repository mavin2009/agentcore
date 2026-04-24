from __future__ import annotations

from agentcore import ChatPromptTemplate, END, START, StateGraph
from agentcore.mcp import MCPServer


def passthrough(state, config):
    return {}


graph = StateGraph(dict, name="mcp_fixture_server", worker_count=1)
graph.add_node("passthrough", passthrough)
graph.add_edge(START, "passthrough")
graph.add_edge("passthrough", END)
compiled = graph.compile()


def upper_tool(request):
    return {"upper": str(request.get("text", "")).upper()}


def sum_tool(request):
    values = list(request.get("values", []))
    return {"total": sum(int(value) for value in values)}


def fail_tool(request):
    return {
        "ok": False,
        "output": {"reason": f"bad::{request.get('value', '')}"},
        "flags": 0,
    }


compiled.tools.register(
    "upper",
    upper_tool,
    decode_input="json",
    metadata={
        "display_name": "Upper Tool",
        "capabilities": ["sync", "async", "structured_request", "structured_response"],
        "request_format": "json",
        "response_format": "json",
    },
)
compiled.tools.register(
    "sum_numbers",
    sum_tool,
    decode_input="json",
    metadata={
        "display_name": "Sum Tool",
        "capabilities": ["sync", "async", "structured_request", "structured_response"],
        "request_format": "json",
        "response_format": "json",
    },
)
compiled.tools.register(
    "fail_tool",
    fail_tool,
    decode_input="json",
    metadata={
        "display_name": "Fail Tool",
        "capabilities": ["sync", "async", "structured_request", "structured_response"],
        "request_format": "json",
        "response_format": "json",
    },
)

server = MCPServer.from_compiled_graph(
    compiled,
    name="agentcore-fixture",
    version="0.1",
    descriptions={
        "upper": "Uppercase text through the AgentCore MCP bridge.",
        "sum_numbers": "Sum integer values through the AgentCore MCP bridge.",
        "fail_tool": "Return a structured tool-level error result.",
    },
    input_schemas={
        "upper": {
            "type": "object",
            "properties": {
                "text": {"type": "string"},
            },
            "required": ["text"],
            "additionalProperties": False,
        },
        "sum_numbers": {
            "type": "object",
            "properties": {
                "values": {
                    "type": "array",
                    "items": {"type": "integer"},
                },
            },
            "required": ["values"],
            "additionalProperties": False,
        },
        "fail_tool": {
            "type": "object",
            "properties": {
                "value": {"type": "string"},
            },
            "required": ["value"],
            "additionalProperties": False,
        },
    },
)


@server.tool(
    "optional_context",
    description="Exercise roots, sampling, elicitation, logging, and resource subscriptions.",
    input_schema={
        "type": "object",
        "properties": {
            "topic": {"type": "string"},
        },
        "required": ["topic"],
        "additionalProperties": False,
    },
)
def optional_context(arguments, metadata):
    session = metadata["mcp"]
    topic = str(arguments.get("topic", "general"))
    roots = session.list_roots()
    session.send_log("debug", {"topic": topic, "phase": "debug"}, logger="fixture.optional")
    session.send_log(
        "warning",
        {"topic": topic, "root_count": len(roots)},
        logger="fixture.optional",
    )
    sampled = session.sample(
        [
            {
                "role": "user",
                "content": {
                    "type": "text",
                    "text": f"Summarize {topic} with {len(roots)} roots.",
                },
            }
        ],
        system_prompt="Keep responses compact.",
        max_tokens=64,
        metadata={"topic": topic},
    )
    elicited = session.elicit(
        f"Provide reviewer context for {topic}.",
        {
            "type": "object",
            "properties": {
                "reviewer": {"type": "string"},
                "approved": {"type": "boolean"},
            },
            "required": ["reviewer", "approved"],
            "additionalProperties": False,
        },
    )
    return {
        "root_count": len(roots),
        "first_root": None if not roots else roots[0]["uri"],
        "sampled_text": sampled["content"].get("text", ""),
        "elicitation": elicited,
        "logging_level": session.logging_level,
        "roots_changed_count": session.roots_change_count,
        "resource_update_notified": session.notify_resource_updated("memo://guide/overview"),
        "subscriptions": list(session.subscribed_resources),
    }


@server.tool(
    "register_dynamic_assets",
    description="Add dynamic MCP tools, prompts, and resources after initialization.",
    input_schema={
        "type": "object",
        "properties": {},
        "additionalProperties": False,
    },
)
def register_dynamic_assets(arguments, metadata):
    if "dynamic_echo" not in {tool["name"] for tool in server.list_tools()}:
        server.add_tool(
            "dynamic_echo",
            lambda request: {"echo": str(request.get("value", ""))},
            description="Echo a dynamic value.",
            input_schema={
                "type": "object",
                "properties": {
                    "value": {"type": "string"},
                },
                "required": ["value"],
                "additionalProperties": False,
            },
        )
    if "dynamic_prompt" not in {prompt["name"] for prompt in server.list_prompts()}:
        server.add_prompt(
            "dynamic_prompt",
            lambda values: {
                "messages": [
                    {
                        "role": "user",
                        "content": {
                            "type": "text",
                            "text": f"Dynamic prompt for {values.get('topic', 'general')}",
                        },
                    }
                ]
            },
            description="A prompt registered after session start.",
            arguments=[
                {"name": "topic", "required": False, "description": "Topic name"},
            ],
        )
    if "memo://guide/dynamic" not in {resource["uri"] for resource in server.list_resources()}:
        server.add_resource(
            "memo://guide/dynamic",
            {
                "text": "Dynamic resource body",
                "mimeType": "text/plain",
                "name": "dynamic",
            },
            name="dynamic",
            description="A resource registered after session start.",
            mime_type="text/plain",
        )
    return {
        "tool_count": len(server.list_tools()),
        "prompt_count": len(server.list_prompts()),
        "resource_count": len(server.list_resources()),
    }


review_prompt = ChatPromptTemplate.from_messages(
    [
        ("system", "You are reviewing {language} code."),
        ("user", "Repository: {repository}\nQuestion: {question}"),
    ],
    name="review_prompt",
)


server.add_prompt_template(
    "review_code",
    review_prompt,
    description="Render a compact code review prompt.",
    arguments=[
        {"name": "language", "required": True, "description": "Programming language"},
        {"name": "repository", "required": True, "description": "Repository name"},
        {"name": "question", "required": True, "description": "Question to answer"},
    ],
    argument_completions={
        "language": ["python", "cpp", "rust"],
    },
)


@server.prompt(
    "investigate_topic",
    description="Return a prompt with embedded resource context.",
    arguments=[
        {"name": "topic", "required": True, "description": "Topic name"},
        {"name": "question", "required": True, "description": "Question to answer"},
    ],
)
def investigate_topic(arguments):
    topic = str(arguments.get("topic", "general"))
    question = str(arguments.get("question", ""))
    return {
        "description": f"Investigation prompt for {topic}",
        "messages": [
            {
                "role": "system",
                "content": {"type": "text", "text": f"You are investigating {topic}."},
            },
            {
                "role": "user",
                "content": {
                    "type": "resource",
                    "resource": {
                        "uri": f"memo://guide/{topic}",
                        "mimeType": "text/plain",
                        "text": f"Context for {topic}: stay deterministic and explicit.",
                    },
                },
            },
            {
                "role": "user",
                "content": {"type": "text", "text": question},
            },
        ],
    }


server.add_resource(
    "memo://guide/overview",
    {
        "text": '{"project":"agentcore","focus":"mcp"}',
        "mimeType": "application/json",
        "name": "overview",
    },
    name="overview",
    description="Overview guide",
    mime_type="application/json",
)


@server.resource_template(
    "memo://guide/{topic}",
    name="topic_guide",
    description="Topic-specific guide",
    mime_type="text/plain",
    argument_completions={
        "topic": ["python", "cpp", "scheduler"],
    },
)
def topic_resource(arguments):
    topic = str(arguments.get("topic", "general"))
    return {
        "text": f"Guide for {topic}: use explicit state patches and deterministic routing.",
        "mimeType": "text/plain",
    }

server.run_stdio()
