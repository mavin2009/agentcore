# AgentCore

<div align="center">
  <img src="https://raw.githubusercontent.com/mavin2009/agentcore/main/assets/agentcore.png" alt="AgentCore logo" width="180" />
  <h3>Native agent graphs for fast, inspectable, long-running workflows.</h3>
  <p>
    AgentCore is a C++20 agent-graph runtime with a compact Python API for stateful workflows,
    persistent subgraphs, structured memory, graph-store hydration, replay, MCP interoperability,
    and OpenTelemetry.
  </p>
  <p>
    <a href="https://pypi.org/project/agentcore-graph/"><img alt="PyPI version" src="https://img.shields.io/pypi/v/agentcore-graph"></a>
    <a href="https://pypi.org/project/agentcore-graph/"><img alt="Python versions" src="https://img.shields.io/pypi/pyversions/agentcore-graph"></a>
    <a href="https://github.com/mavin2009/agentcore/blob/main/LICENSE"><img alt="License" src="https://img.shields.io/github/license/mavin2009/agentcore"></a>
    <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-blue">
  </p>
  <p>
    <a href="#install">Install</a>
    · <a href="#quick-start">Quick Start</a>
    · <a href="./docs/quickstarts/python.md">Python Guide</a>
    · <a href="./docs/reference/api.md">API Reference</a>
    · <a href="./docs/comparisons/langgraph-head-to-head.md">Benchmarks</a>
    · <a href="./docs/README.md">Docs</a>
  </p>
</div>

AgentCore is designed for graph-shaped agent systems where latency, durability, and state visibility matter at the same time. The runtime keeps graph execution native, makes state mutation explicit through patches, and records checkpoints and traces from the same execution path used by normal runs.

The Python surface is intentionally familiar: define a `StateGraph`, add nodes and edges, compile it, and invoke it. The difference is underneath. Graph metadata, state commits, scheduling, checkpointing, message merging, persistent subgraph sessions, and supported intelligence queries run through the native runtime instead of being rebuilt in Python at each step.

AgentCore is an independent project. It is not affiliated with Amazon Web Services, AWS AgentCore, or any related AWS-branded product or service.

## Start Here

<table>
  <tr>
    <td><strong>Use it from Python</strong></td>
    <td><a href="#install">Install</a>, then follow the <a href="#quick-start">quick start</a> or the full <a href="./docs/quickstarts/python.md">Python guide</a>.</td>
  </tr>
  <tr>
    <td><strong>Migrate graph code</strong></td>
    <td>Use the <a href="./docs/migration/langgraph-to-agentcore.md">migration guide</a> for builder patterns, reducers, message state, and current compatibility notes.</td>
  </tr>
  <tr>
    <td><strong>Understand the runtime</strong></td>
    <td>Read the <a href="./docs/concepts/runtime-model.md">runtime model</a> for state patches, scheduler behavior, checkpointing, sessions, intelligence records, and knowledge-graph state.</td>
  </tr>
  <tr>
    <td><strong>Integrate with tools</strong></td>
    <td>See <a href="./docs/integrations/mcp.md">MCP</a>, <a href="./docs/integrations/graph-stores.md">graph stores</a>, <a href="./docs/integrations/opentelemetry.md">OpenTelemetry</a>, and the <a href="./docs/reference/api.md">API reference</a>.</td>
  </tr>
  <tr>
    <td><strong>Validate performance</strong></td>
    <td>Use the <a href="./docs/operations/validation.md">validation guide</a> and current <a href="./docs/comparisons/langgraph-head-to-head.md">benchmark notes</a>.</td>
  </tr>
</table>

<p align="center">
  <img src="https://raw.githubusercontent.com/mavin2009/agentcore/main/assets/arch.png" alt="AgentCore architecture" width="820" />
</p>

## Why This Exists

Many agent workflows start simple and then become expensive in the wrong places: message history grows, branches join, tools wait, subgraphs need memory, and replay becomes a separate problem from execution. AgentCore tries to keep those concerns in one runtime model.

The core design choices are:

- compiled graph metadata instead of ad hoc per-step graph interpretation
- explicit state patches instead of hidden shared mutation
- typed hot state plus blob references for larger payloads
- deterministic commit ordering for branches, joins, checkpoints, and replay
- persistent child sessions for reusable subgraphs without shared mutable child state
- native knowledge-graph and intelligence records for workflows that need structured context
- opt-in observability and durability profiles so metadata does not always dominate the hot path

This does not make AgentCore a planner, prompt framework, vector database, or agent marketplace. It is the execution layer those pieces can sit on.

## Install

```bash
python3 -m pip install agentcore-graph
```

The package published to PyPI is `agentcore-graph`; the Python import package is `agentcore`.

For OpenTelemetry dependencies:

```bash
python3 -m pip install "agentcore-graph[otel]"
```

For optional Neo4j graph-store support:

```bash
python3 -m pip install "agentcore-graph[neo4j]"
```

The package also installs MCP helper commands:

```bash
agentcore-mcp
agentcore-mcp-server
agentcore-mcp-config
```

Current published wheels target Linux `x86_64` for CPython `3.9` through `3.12`. Source builds remain available from this repository.

## Quick Start

```python
from agentcore.graph import END, START, StateGraph


def step(state, config):
    return {"count": int(state.get("count", 0)) + 1}


def route(state, config):
    return END if state["count"] >= 3 else "step"


graph = StateGraph(dict, name="counter", worker_count=2)
graph.add_node("step", step)
graph.add_edge(START, "step")
graph.add_conditional_edges("step", route, {END: END, "step": "step"})

compiled = graph.compile()
final_state = compiled.invoke({"count": 0})
print(final_state["count"])
```

From the same compiled graph you can also stream events, inspect metadata, pause and resume, register tools and models, mirror MCP tools, emit OpenTelemetry spans, and compose persistent subgraphs.

Useful next reads:

- [Python quickstart](./docs/quickstarts/python.md)
- [API reference](./docs/reference/api.md)
- [Runtime model](./docs/concepts/runtime-model.md)

## What Is Implemented

AgentCore currently includes:

- native C++ runtime with Python bindings
- `StateGraph`-style Python builder with conditional edges, joins, subgraphs, streaming, batch execution, and pause/resume
- native join reducers for list concatenation and ID-aware message history merging
- multi-worker scheduling with async wait handling
- checkpointing, trace events, proof digests, and replay-oriented metadata
- persistent subgraph sessions with isolated child state and deterministic session revisions
- structured intelligence state for tasks, claims, evidence, decisions, and memories
- graph-native context assembly from messages, intelligence records, native knowledge-graph triples, and state fields
- knowledge-graph-backed state and reactive execution hooks
- external graph-store hydration with an in-memory reference backend and optional Neo4j adapter
- deterministic memoization for supported pure nodes
- prompt templates for text, chat, and MCP-rendered prompts
- tool and model registries with built-in HTTP JSON, SQLite-style, local model, OpenAI-compatible, xAI Grok, and Gemini adapters
- MCP over `stdio`, including tools, prompts, resources, completions, roots, sampling, elicitation, logging, subscriptions, and installable MCP server launchers
- opt-in OpenTelemetry spans and metrics
- native and Python benchmark and smoke-test coverage

## Core Python Features

### Message State

Agent workflows often keep conversation history in a `messages` field. AgentCore exposes `MessagesState` and `add_messages` so message joins can run in the native merge path: messages with matching non-empty `id` values replace earlier messages in place, while messages without ids append.

```python
from agentcore.graph import MessagesState, StateGraph


class AgentState(MessagesState, total=False):
    summary: str


graph = StateGraph(AgentState, name="agent", worker_count=4)
```

See the [Python quickstart](./docs/quickstarts/python.md#use-message-state) and [API reference](./docs/reference/api.md#stategraph).

### Persistent Subgraphs

Persistent subgraph sessions let the same child graph run across many `session_id` values with isolated child state, checkpoints, task journal, knowledge graph, and stream metadata. Distinct sessions can run concurrently; concurrent reuse of the same session is rejected deterministically.

See the [runtime model](./docs/concepts/runtime-model.md#persistent-session-lifecycle) and [Python quickstart](./docs/quickstarts/python.md#persistent-subgraph-sessions).

### Context Assembly

Nodes can declare a `ContextSpec` and then call `runtime.context.view()` to assemble a deterministic context view from message history, intelligence records, native knowledge-graph triples, selected state fields, and optional graph-shaped state carried by the workflow. The returned `ContextView` includes citations, provenance, budget stats, conflict metadata, prompt/message rendering helpers, and a stable digest that is surfaced through `invoke_with_metadata(...)`.

```python
from agentcore.graph import ContextSpec


graph.add_node(
    "answer",
    answer,
    context=ContextSpec(
        goal_key="question",
        include=["messages.recent", "claims.supported", "evidence.relevant"],
        budget_tokens=2400,
        require_citations=True,
    ),
)


def answer(state, config, runtime):
    context = runtime.context.view()
    prompt = context.to_prompt(system="Answer using cited evidence.")
    result = runtime.invoke_model("default", prompt, decode="text")
    return {"answer": result, "context_digest": context.digest}
```

See the [Python quickstart](./docs/quickstarts/python.md#assemble-context-for-a-node) and [runtime model](./docs/concepts/runtime-model.md#context-assembly).

### Intelligence State

The intelligence layer stores operational records inside runtime state:

- tasks
- claims
- evidence
- decisions
- memories

Python nodes use `runtime.intelligence` to write and query those records. The records participate in normal patch commits, checkpoints, replay, and persistent subgraph sessions.

```python
def analyze(state, config, runtime):
    runtime.intelligence.upsert_task(
        "triage",
        title="Triage incoming request",
        owner="planner",
        status="in_progress",
        priority=5,
    )
    runtime.intelligence.upsert_claim(
        "needs-followup",
        subject="request",
        relation="requires",
        object="followup",
        confidence=0.72,
    )
    next_task = runtime.intelligence.next_task(owner="planner")
    return {"next_task": None if next_task is None else next_task["key"]}
```

See the [runtime model](./docs/concepts/runtime-model.md#intelligence-state) and [Python guide](./docs/quickstarts/python.md#use-the-intelligence-state-model).

### External Graph Stores

Native knowledge-graph state is useful during execution because it participates in patches, checkpoints, context assembly, and replay. When the source of truth lives elsewhere, AgentCore lets a node explicitly hydrate runtime knowledge from a registered graph store, then optionally sync selected runtime triples back out.

```python
from agentcore.graph import END, START, StateGraph


def retrieve(state, config, runtime):
    loaded = runtime.knowledge.load_neighborhood(
        "Incident",
        store="ops_graph",
        depth=2,
        limit=20,
    )
    return {"loaded_triples": len(loaded["triples"])}


graph = StateGraph(dict, name="ops_graph_demo")
graph.add_node("retrieve", retrieve)
graph.add_edge(START, "retrieve")
graph.add_edge("retrieve", END)

compiled = graph.compile()
compiled.graph_stores.register_memory(
    "ops_graph",
    triples=[("Incident", "affects", "checkout API")],
)
```

The first shipped backends are `InMemoryGraphStore` and `Neo4jGraphStore`. The connector contract is intentionally entity/triple/neighborhood based so other graph databases can implement the same surface without changing the runtime.

The Neo4j adapter has an optional live Docker validation path in addition to the default dependency-free graph-store smoke test. That path exercises batch writes, neighborhood reads, filtered queries, runtime hydration, context assembly, and sync-back persistence against a real Neo4j process.

See the [graph-store integration guide](./docs/integrations/graph-stores.md), [Python guide](./docs/quickstarts/python.md#hydrate-knowledge-from-an-external-graph-store), and [runtime model](./docs/concepts/runtime-model.md#external-graph-stores).

### MCP Interoperability

AgentCore can consume MCP servers and expose AgentCore-owned tools, prompts, resources, and graph surfaces through MCP.

```python
compiled.tools.register_mcp_stdio(
    ["python3", "./my_mcp_server.py"],
    prefix="remote",
)
```

Installed helper commands can also produce client configuration snippets for common MCP clients:

```bash
agentcore-mcp-config claude --name local-agentcore --target ./server.py:build_server
agentcore-mcp-config codex --name local-agentcore --target ./server.py:build_server
agentcore-mcp-config gemini --name local-agentcore --target ./server.py:build_server
```

See the [MCP integration guide](./docs/integrations/mcp.md).

### OpenTelemetry

Telemetry is opt-in. Passing `telemetry=True` or an `OpenTelemetryObserver` emits run spans, optional node spans, and run/node metrics using the Python OpenTelemetry API.

```python
from agentcore.observability import OpenTelemetryObserver


observer = OpenTelemetryObserver()
details = compiled.invoke_with_metadata({"count": 0}, telemetry=observer)
```

See the [OpenTelemetry guide](./docs/integrations/opentelemetry.md).

## Performance

AgentCore's performance work is focused on native graph execution, branch/join overhead, persistent subgraph sessions, resume behavior, and structured state operations. Current benchmark snapshots and exact reproduction commands live in [the comparison document](./docs/comparisons/langgraph-head-to-head.md).

The latest snapshot in this repository was generated on April 22, 2026. On that machine and workload set, AgentCore's compatibility surface was about `2.22x` to `3.01x` faster on the same-code builder path, while native persistent-session workloads were about `2.50x` to `13.11x` faster with lower measured memory use. Treat those as workload-specific measurements, not universal claims.

For local validation, start with:

```bash
ctest --test-dir build --output-on-failure
python3 python/benchmarks/langgraph_head_to_head.py
```

See the [validation guide](./docs/operations/validation.md) for release, sanitizer, replay, and benchmark commands.

## Related Ideas And Papers

AgentCore is an implementation project, not a research paper. Its design is influenced by several lines of work and industry practice:

- [Context graphs](https://foundationcapital.com/ideas/context-graphs-ais-trillion-dollar-opportunity) argue that production agents need durable, connected decision context rather than only chat history. AgentCore's intelligence records and knowledge-graph state are a runtime substrate for that style of workflow, with explicit commits and replay.
- [ReAct](https://arxiv.org/abs/2210.03629) frames agents as interleaved reasoning and action. In AgentCore, that loop is represented as graph structure, node outputs, tool/model calls, and inspectable state transitions.
- [Reflexion](https://arxiv.org/abs/2303.11366) explores feedback and memory for language agents. AgentCore's memories, decisions, evidence, and persistent sessions give applications a structured place to store those artifacts.
- [Retrieval-Augmented Generation](https://arxiv.org/abs/2005.11401) and [GraphRAG](https://arxiv.org/abs/2404.16130) motivate external knowledge and graph-shaped retrieval. AgentCore does not replace a retrieval system, but it can carry retrieved evidence, claims, and graph memory through a deterministic workflow.
- [Pregel](https://research.google/pubs/pregel-a-system-for-large-scale-graph-processing/) and [Apache Beam](https://beam.apache.org/) inform the broader design language around graph execution, deterministic progress, durable state, and replayable dataflow.

## Build From Source

For a normal local build:

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For optimized validation and benchmarks:

```bash
cmake --preset release-perf
cmake --build --preset release-perf -j
ctest --preset release-perf
./build/release-perf/agentcore_runtime_benchmark
./build/release-perf/agentcore_persistent_subgraph_session_benchmark
```

The repository also includes `relwithdebinfo-perf`, `asan`, `ubsan`, and `tsan` presets.

## Documentation Map

The main docs index is [docs/README.md](./docs/README.md).

| Area | Document |
| --- | --- |
| Python workflow authoring | [docs/quickstarts/python.md](./docs/quickstarts/python.md) |
| Native C++ embedding | [docs/quickstarts/cpp.md](./docs/quickstarts/cpp.md) |
| Runtime semantics | [docs/concepts/runtime-model.md](./docs/concepts/runtime-model.md) |
| Python and C++ API surface | [docs/reference/api.md](./docs/reference/api.md) |
| MCP integration | [docs/integrations/mcp.md](./docs/integrations/mcp.md) |
| Graph stores | [docs/integrations/graph-stores.md](./docs/integrations/graph-stores.md) |
| OpenTelemetry | [docs/integrations/opentelemetry.md](./docs/integrations/opentelemetry.md) |
| Migration notes | [docs/migration/langgraph-to-agentcore.md](./docs/migration/langgraph-to-agentcore.md) |
| Validation and benchmarks | [docs/operations/validation.md](./docs/operations/validation.md) |
| Head-to-head benchmark notes | [docs/comparisons/langgraph-head-to-head.md](./docs/comparisons/langgraph-head-to-head.md) |

## Repository Map

- `agentcore/include` and `agentcore/src`: native runtime
- `agentcore/adapters`: built-in tool and model adapters
- `agentcore/tests` and `agentcore/benchmarks`: native tests and benchmark entry points
- `python/agentcore`: Python package layered over the native module
- `python/tests` and `python/benchmarks`: Python smoke tests and benchmark entry points
- `docs`: guides, concepts, reference material, migration notes, and validation docs
- `assets`: README and documentation images

## Acknowledgements

AgentCore is independent and is not affiliated with or endorsed by LangChain Inc.

I am grateful to the projects and ideas that helped clarify the design space for graph-oriented agent runtimes:

- [LangGraph](https://github.com/langchain-ai/langgraph) and its [documentation](https://docs.langchain.com/langgraph) for making graph-based agent orchestration concrete and accessible
- [NetworkX](https://networkx.org/) for graph-first modeling vocabulary
- [Model Context Protocol](https://modelcontextprotocol.io/) for a practical interoperability layer for tools, prompts, and resources
- [OpenTelemetry](https://opentelemetry.io/) for the standard observability vocabulary AgentCore plugs into

## License

This repository is licensed under the MIT License. See [LICENSE](./LICENSE) and [NOTICE](./NOTICE).
