# AgentCore Documentation

This directory is meant to help you move from "what is this?" to "I can ship a graph with it" without reading the whole repository.

If you are new to AgentCore, start with the path that matches what you are trying to do. The deeper reference pages are still here, but most users should not need to read them front to back on day one.

## Choose Your Path

| I want to... | Start here | What you will learn |
| --- | --- | --- |
| Build a graph from Python | [Python quickstart](./quickstarts/python.md) | Install/build, define nodes, route, stream, inspect metadata, and use common agent features |
| Move an existing graph workflow | [Migration guide](./migration/langgraph-to-agentcore.md) | Import changes, callback differences, reducer support, message state, and current compatibility limits |
| Understand why the runtime behaves this way | [Runtime model](./concepts/runtime-model.md) | State patches, scheduler behavior, checkpoints, subgraph sessions, intelligence records, and knowledge-graph state |
| Connect external tools and context | [MCP integration](./integrations/mcp.md) | Mirror MCP tools, consume prompts/resources, expose AgentCore as an MCP server, and generate client config |
| Export traces and metrics | [OpenTelemetry integration](./integrations/opentelemetry.md) | Opt-in run/node spans, metrics, and how to keep telemetry out of the default hot path |
| Embed the runtime from C++ | [C++ quickstart](./quickstarts/cpp.md) | Native build, `ExecutionEngine`, state patches, and executable examples |
| Prove a change still works | [Validation guide](./operations/validation.md) | Smoke tests, release builds, benchmarks, wheel checks, replay gates, and suggested validation lanes |
| Check performance claims | [Benchmark notes](./comparisons/langgraph-head-to-head.md) | Current measured workloads, commands, environment details, and how to rerun the comparison |

## Recommended First Read

For most Python users:

1. Read the first two sections of the [Python quickstart](./quickstarts/python.md).
2. Run the minimal counter graph.
3. Skim [Message State](./quickstarts/python.md#use-message-state) if you are building a chat-style agent.
4. Skim [Persistent Subgraph Sessions](./quickstarts/python.md#persistent-subgraph-sessions) if your workflow has reusable specialist or child-agent state.
5. Use the [API reference](./reference/api.md) only when you need exact signatures.

For runtime or platform engineers:

1. Read [Runtime Model](./concepts/runtime-model.md).
2. Run the native validation path in [Validation And Benchmarks](./operations/validation.md).
3. Inspect the public headers under `../agentcore/include/agentcore`.

## Practical Entry Points

- [Python quickstart](./quickstarts/python.md): the main tutorial surface.
- [API reference](./reference/api.md): exact Python and C++ surfaces.
- [Runtime model](./concepts/runtime-model.md): execution semantics and state behavior.
- [MCP integration](./integrations/mcp.md): external tool, prompt, and resource interoperability.
- [OpenTelemetry integration](./integrations/opentelemetry.md): observability setup and emitted attributes.
- [Migration guide](./migration/langgraph-to-agentcore.md): porting existing graph-builder code.
- [Validation guide](./operations/validation.md): confidence checks before publishing or merging.

## Examples And Tests Worth Reading

The docs are backed by executable material in the repository. When in doubt, the tests are the most precise examples of current behavior.

- Python graph smoke: [`../python/tests/state_graph_api_smoke.py`](../python/tests/state_graph_api_smoke.py)
- Agent workflow smoke: [`../python/tests/agent_workflows_smoke.py`](../python/tests/agent_workflows_smoke.py)
- MCP smoke: [`../python/tests/mcp_runtime_smoke.py`](../python/tests/mcp_runtime_smoke.py)
- OpenTelemetry smoke: [`../python/tests/opentelemetry_smoke.py`](../python/tests/opentelemetry_smoke.py)
- Native planner example: [`../agentcore/examples/planner_executor_graph.cpp`](../agentcore/examples/planner_executor_graph.cpp)
- Native knowledge-graph example: [`../agentcore/examples/knowledge_graph_workflow.cpp`](../agentcore/examples/knowledge_graph_workflow.cpp)
- Proof/replay example: [`../agentcore/examples/runtime_proof_suite.cpp`](../agentcore/examples/runtime_proof_suite.cpp)

## How The Docs Are Organized

The README is the front door. The quickstarts are for getting something working. The concept docs explain behavior. The reference docs are for exact signatures. The operations docs are for proving changes and reproducing measurements.

That split is deliberate: AgentCore has advanced runtime features, but users should be able to approach them in layers instead of being dropped straight into engine internals.
