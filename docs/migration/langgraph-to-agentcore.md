# LangGraph To AgentCore

This guide is the shortest path from a LangGraph-style `StateGraph` to AgentCore.

Use it when you already have graph-builder code and want to answer three practical questions:

- How much code can stay structurally the same?
- Which behavior should move to AgentCore-native APIs?
- What should I test before trusting the port?

There are two migration modes:

1. Local side-by-side evaluation
   Keep the StateGraph mental model and use the generated compatibility surface from a local AgentCore build.

2. Native AgentCore adoption
   Switch imports to `agentcore.graph` and pick up persistent subgraph sessions, event-first streaming, explicit pause/resume, and proof/checkpoint surfaces.

## Fastest Path

If your LangGraph code already looks like this:

```python
from langgraph.graph import StateGraph, START, END
from langgraph.types import Command
```

the stable long-term AgentCore import is:

```python
from agentcore.graph import StateGraph, START, END, Command
```

In many simple graphs, the rest of the code can stay structurally similar.

## Local Evaluation With Near-Zero Graph Changes

When you build AgentCore with Python bindings, the build emits a generated compatibility package under `./build/python/langgraph`.

That means you can do a local benchmark pass against AgentCore's backend with your existing `langgraph.graph` imports by running your script like this:

```bash
cmake -S . -B build -DAGENTCORE_BUILD_PYTHON_BINDINGS=ON
cmake --build build -j
PYTHONPATH=./build/python python3 your_existing_langgraph_script.py
```

This path is useful for:

- quick side-by-side performance checks
- verifying that a basic graph still behaves the same
- estimating migration effort before changing imports

Today that compatibility surface is best treated as an evaluation seam, not the full long-term public API.

It currently targets core `StateGraph` builder flows such as `invoke()`, `stream()`, and `batch()`, not every LangGraph surface area.

## Recommended Migration Order

For a real application, migrate in layers:

1. Run your existing graph against the local compatibility surface if you want a fast first signal.
2. Switch imports to `agentcore.graph`.
3. Keep node bodies unchanged at first unless they depend on framework-specific interrupt, checkpoint, or store behavior.
4. Add explicit tests for final state, streamed events, and pause/resume behavior.
5. Move message history to `MessagesState` or `add_messages`.
6. Move reusable child-agent state to persistent subgraph sessions.
7. Add OpenTelemetry or MCP only after the graph behavior is stable.

That order keeps the migration honest: first preserve behavior, then adopt the native features that are difficult to bolt on after the fact.

## Native Import Change

The explicit import-level migration is small:

```python
# LangGraph
from langgraph.graph import StateGraph, START, END
from langgraph.types import Command

# AgentCore
from agentcore.graph import StateGraph, START, END, Command
```

You can usually also add an explicit worker count at graph construction time:

```python
graph = StateGraph(dict, name="research", worker_count=4)
```

## Minimal Before And After

Before:

```python
from langgraph.graph import END, START, StateGraph


def draft(state):
    return {"answer": "draft"}


graph = StateGraph(dict)
graph.add_node("draft", draft)
graph.add_edge(START, "draft")
graph.add_edge("draft", END)
app = graph.compile()
```

After:

```python
from agentcore.graph import END, START, StateGraph


def draft(state, config):
    return {"answer": "draft"}


graph = StateGraph(dict, name="research", worker_count=4)
graph.add_node("draft", draft)
graph.add_edge(START, "draft")
graph.add_edge("draft", END)
app = graph.compile()
```

The graph shape is the same. The native AgentCore version adds an explicit name and worker count so the runtime metadata and scheduling behavior are easier to inspect.

## What Usually Stays The Same

- The graph-builder mental model: `add_node(...)`, `add_edge(...)`, `add_conditional_edges(...)`, `compile()`.
- State-as-dict workflows.
- Node functions that return partial state updates as dictionaries.
- Command-style routing with `goto`.
- Batch and streaming entry points.
- Sequence-style builder flows such as `add_sequence(...)`, `set_finish_point(...)`, and conditional entry routing.

## What Usually Changes

### 1. Imports

```python
from agentcore.graph import StateGraph, START, END, Command
```

### 2. Callback Signature

AgentCore callbacks may accept:

- `state`
- `(state, config)`
- `(state, config, runtime)`

The optional `runtime` argument exposes native services such as `RuntimeContext.record_once(...)`.

Named callables can also be registered without repeating the node name:

```python
graph.add_node(planner_step)
graph.add_sequence([planner_step, reviewer_step])
```

Lambdas and anonymous callables still need explicit names.

### 3. Pause / Resume

LangGraph-style interrupt flows usually become explicit AgentCore wait flows.

LangGraph:

```python
from langgraph.types import interrupt, Command


def gate(state):
    approved = interrupt("approve?")
    return {"approved": bool(approved)}
```

AgentCore:

```python
from agentcore.graph import Command


def gate(state, config):
    if not state.get("approved", False):
        return Command(update={"approved": True}, wait=True)
    return {"ready": True}
```

Then resume through:

```python
paused = compiled.invoke_until_pause_with_metadata({"input": 7})
resumed = compiled.resume_with_metadata(paused["summary"]["checkpoint_id"])
```

### 4. Streaming

AgentCore's native Python API is event-first:

```python
events = list(compiled.stream({"count": 0}))
details = compiled.invoke_with_metadata({"count": 0})
```

Streamed and traced events may carry:

- `namespaces`
- `session_id`
- `session_revision`

That is especially useful once you start using persistent subgraphs or nested graphs.

### 4a. OpenTelemetry

If your existing application already exports OpenTelemetry data, AgentCore can attach to that stack without changing the graph shape itself:

```python
from agentcore.observability import OpenTelemetryObserver


observer = OpenTelemetryObserver()
details = compiled.invoke_with_metadata({"count": 0}, telemetry=observer)
```

For a quick first pass, `telemetry=True` is also accepted on `invoke(...)`, `invoke_with_metadata(...)`, `stream(...)`, pause, resume, and batch helpers.

### 5. Builder Conveniences

The native `agentcore.graph.StateGraph` now accepts several migration-friendly builder patterns directly:

```python
graph.add_sequence([("draft", draft), ("review", review)])
graph.set_finish_point("review")
graph.set_conditional_entry_point(route_start, {"draft": "draft", "fallback": "fallback"})
graph.add_edge(["planner", "critic"], "synthesize")
```

Those patterns are intentionally additive. They keep the Python surface close to familiar graph-builder flows while still compiling into AgentCore's native execution model.

### 6. Schema-Driven Reducers

If your state schema already uses `TypedDict` plus `Annotated[...]` reducers, AgentCore can now infer the reducer subset that maps directly to the native join engine:

```python
import operator
from typing import Annotated, TypedDict


class ReviewState(TypedDict, total=False):
    score: Annotated[int, operator.add]
    notes: Annotated[list[str], operator.add]
    approved: Annotated[bool, operator.or_]
```

For agent message history, use the built-in message reducer:

```python
from agentcore.graph import MessagesState, add_messages


class AgentState(MessagesState, total=False):
    summary: str
```

Today the inferred subset is:

- `Annotated[int, operator.add]`
- `Annotated[int, min]`
- `Annotated[int, max]`
- `Annotated[list[T], operator.add]`
- `Annotated[list[dict], add_messages]`
- `Annotated[bool, operator.or_]`
- `Annotated[bool, operator.and_]`

Those reducers are applied automatically when a node becomes a join barrier. List concatenation runs in the native join engine by concatenating tagged sequence blobs, so it preserves deterministic branch order without calling back into Python at the barrier. Message merging uses a native message blob: messages with matching non-empty `id` values replace earlier messages in place, while messages without ids append. If your schema depends on reducer functions outside that subset, keep the migration explicit with `merge={...}` or a dedicated aggregation node.

### 7. Subgraphs

Instead of manually calling a child graph from inside a parent node, AgentCore can make subgraphs explicit:

```python
parent.add_subgraph(
    "specialist_session",
    specialist_graph,
    namespace="specialist_session",
    inputs={"specialist_id": "session_id", "question": "query"},
    outputs={"answer": "answer", "visits": "visits"},
    session_mode="persistent",
    session_id_from="specialist_id",
)
```

That gives you:

- explicit parent-to-child bindings
- isolated child-session snapshots
- built-in session reuse
- namespaced stream visibility
- deterministic same-session conflict rejection

When the parent and child already share a declared state schema, there is also a shorter path:

```python
parent.add_node("specialist_child", specialist_graph)
```

That direct form binds the overlapping schema fields by name for both input and output. It is intentionally the shared-state default only. Use `add_subgraph(...)` when you need explicit bindings, namespaces, persistent sessions, or knowledge-graph propagation rules.

### 8. External Tool Ecosystem

If part of your existing stack already exposes tools through MCP, AgentCore can mirror those tools into the graph-owned registry instead of forcing a parallel tool-integration path:

```python
compiled.tools.register_mcp_stdio(
    ["python3", "./python/tests/fixtures/mcp_stdio_server.py"],
    prefix="remote",
)
```

After registration, graph nodes keep using the same `runtime.invoke_tool(...)` surface they already use for built-in or Python-backed adapters. If you also need prompt or context interop, the direct `agentcore.mcp.StdioMCPClient` surface now covers prompt retrieval, resource reads, completions, roots, sampling, elicitation, logging control, and resource subscriptions in addition to tool calls over `stdio`.

## Minimal Example

LangGraph:

```python
from langgraph.graph import StateGraph, START, END


def step(state):
    return {"count": int(state.get("count", 0)) + 1}


graph = StateGraph(dict)
graph.add_node("step", step)
graph.add_edge(START, "step")
graph.add_edge("step", END)
compiled = graph.compile()
result = compiled.invoke({"count": 0})
```

AgentCore:

```python
from agentcore.graph import StateGraph, START, END


def step(state, config):
    return {"count": int(state.get("count", 0)) + 1}


graph = StateGraph(dict, worker_count=2)
graph.add_node("step", step)
graph.add_edge(START, "step")
graph.add_edge("step", END)
compiled = graph.compile()
result = compiled.invoke({"count": 0})
```

Using the migration-friendly builder helpers, the same shape can also be written as:

```python
from agentcore.graph import StateGraph


def step(state, config):
    return {"count": int(state.get("count", 0)) + 1}


graph = StateGraph(dict, worker_count=2)
graph.add_node(step)
graph.set_entry_point("step")
graph.set_finish_point("step")
compiled = graph.compile()
```

## Common Translation Table

| LangGraph pattern | AgentCore pattern |
| --- | --- |
| `StateGraph(...)` | `StateGraph(..., worker_count=N)` |
| `add_node(step_fn)` | same for named callables |
| `add_sequence([...])` | same |
| `set_finish_point("x")` | same |
| `set_conditional_entry_point(...)` | same |
| `add_edge(["a", "b"], "join")` | same builder shape, compiled as an explicit join target |
| `TypedDict` + supported `Annotated[...]` reducers | same reducer intent for native join barriers |
| `MessagesState` / `add_messages` message history | same import from `agentcore.graph`; compiled to native ID-aware message merge |
| `add_node("child", child_graph)` with shared declared schema | supported as shared-state subgraph shorthand |
| `dict` state updates | same |
| `Command(update=..., goto=...)` | same shape |
| `interrupt()` + resume command | `Command(wait=True)` + `invoke_until_pause_with_metadata(...)` / `resume_with_metadata(...)` |
| manual child graph invocation + `thread_id` persistence | `add_subgraph(..., session_mode="persistent", session_id_from=...)` |
| ad hoc stream inspection | `stream(...)` and `invoke_with_metadata(...)` with namespace and session metadata |
| OpenTelemetry export | `telemetry=True` or `OpenTelemetryObserver()` on compiled graph execution methods |

## Compile-Time Differences

`compile(...)` accepts `name=` and `debug=` for migration convenience, but some LangGraph-specific persistence arguments remain explicit differences today.

- `checkpointer=`
- `interrupt_before=`
- `interrupt_after=`
- `store=`

Those currently raise a clear `NotImplementedError` instead of being silently ignored. The intent is to keep the migration seam honest: if a workflow depends on those exact semantics, the port should move to AgentCore's native wait/resume, metadata, and persistent subgraph session surfaces rather than appearing to work while dropping behavior.

## Recommended Migration Order

1. Run your existing graph against the generated compatibility layer from `./build/python`.
2. Switch imports to `agentcore.graph`.
3. Keep returning dict patches and `Command(...)` values.
4. Move chat history fields to `MessagesState` or `Annotated[list[dict], add_messages]` when you need ID-aware message merging.
5. Replace manual child-graph persistence with `add_subgraph(...)`.
6. Replace interrupt-style flows with explicit wait/resume surfaces where needed.
6. Add `invoke_with_metadata(...)` or `stream(...)` once you want better observability.
7. If your application already has an OpenTelemetry stack, add `telemetry=True` or `OpenTelemetryObserver()` after the behavior is stable.

## Related Pages

- Comparison benchmarks: [`../comparisons/langgraph-head-to-head.md`](../comparisons/langgraph-head-to-head.md)
- Python quickstart: [`../quickstarts/python.md`](../quickstarts/python.md)
- API map: [`../reference/api.md`](../reference/api.md)
