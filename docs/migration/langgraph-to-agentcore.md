# LangGraph To AgentCore

This guide is the shortest path from a LangGraph-style `StateGraph` to AgentCore.

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

## What Usually Stays The Same

- The graph-builder mental model: `add_node(...)`, `add_edge(...)`, `add_conditional_edges(...)`, `compile()`.
- State-as-dict workflows.
- Node functions that return partial state updates as dictionaries.
- Command-style routing with `goto`.
- Batch and streaming entry points.

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

### 5. Subgraphs

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

## Common Translation Table

| LangGraph pattern | AgentCore pattern |
| --- | --- |
| `StateGraph(...)` | `StateGraph(..., worker_count=N)` |
| `dict` state updates | same |
| `Command(update=..., goto=...)` | same shape |
| `interrupt()` + resume command | `Command(wait=True)` + `invoke_until_pause_with_metadata(...)` / `resume_with_metadata(...)` |
| manual child graph invocation + `thread_id` persistence | `add_subgraph(..., session_mode="persistent", session_id_from=...)` |
| ad hoc stream inspection | `stream(...)` and `invoke_with_metadata(...)` with namespace and session metadata |

## Recommended Migration Order

1. Run your existing graph against the generated compatibility layer from `./build/python`.
2. Switch imports to `agentcore.graph`.
3. Keep returning dict patches and `Command(...)` values.
4. Replace manual child-graph persistence with `add_subgraph(...)`.
5. Replace interrupt-style flows with explicit wait/resume surfaces where needed.
6. Add `invoke_with_metadata(...)` or `stream(...)` once you want better observability.

## Related Pages

- Comparison benchmarks: [`../comparisons/langgraph-head-to-head.md`](../comparisons/langgraph-head-to-head.md)
- Python quickstart: [`../quickstarts/python.md`](../quickstarts/python.md)
- API map: [`../reference/api.md`](../reference/api.md)
