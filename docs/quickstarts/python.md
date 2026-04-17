# Python Quickstart

This guide uses the local native build produced by CMake. The Python API is a compact builder layered over the native runtime, so the graph definition happens in Python while execution, scheduling, patch application, streaming, and subgraph runtime stay in C++.

## Build The Python Package

From the repository root:

```bash
cmake -S . -B build -DAGENTCORE_BUILD_PYTHON_BINDINGS=ON
cmake --build build -j
```

The build emits a package under `./build/python/agentcore`. To use it directly from the build tree:

```bash
PYTHONPATH=./build/python python3 -c "from agentcore.graph import StateGraph; print('ok')"
```

## Define A Minimal Graph

```python
from agentcore.graph import END, START, StateGraph


def step(state, config):
    next_count = int(state.get("count", 0)) + 1
    return {"count": next_count}


def route(state, config):
    return END if state["count"] >= 3 else "step"


graph = StateGraph(dict, name="counter", worker_count=2)
graph.add_node("step", step)
graph.add_edge(START, "step")
graph.add_conditional_edges("step", route, {END: END, "step": "step"})

compiled = graph.compile()
final_state = compiled.invoke({"count": 0})
```

The important pieces are:

- `StateGraph(...)` defines the graph and the native worker count used when it is compiled.
- `add_node(...)` registers a node callback. The callback may be synchronous or async.
- `add_edge(...)` and `add_conditional_edges(...)` describe routing.
- `compile()` lowers the graph into the native runtime.
- `invoke()` executes one run and returns the final state mapping.

## Inspect Execution

The compiled graph supports three useful inspection surfaces:

- `invoke_with_metadata(...)` returns the final state plus a summary, trace, and proof digest.
- `stream(...)` yields public execution events in run order.
- `astream(...)` is the async wrapper for streamed events.

Example:

```python
details = compiled.invoke_with_metadata({"count": 0})
events = list(compiled.stream({"count": 0}))
```

Today the Python stream surface supports `stream_mode="events"` only.

## Node Return Forms

Python node callbacks can return any of the following:

- a mapping of field updates
- `None`
- a node name string
- `Command(update=..., goto=...)`
- a `(updates, goto)` tuple
- an awaitable that resolves to one of the above

Callbacks may accept either `state` or `(state, config)`. The runtime normalizes the result into a native state patch and optional routing override.

## Fan-Out, Join, And Subgraphs

The builder includes first-class helpers for control-flow shapes that would otherwise require a lot of boilerplate:

- `add_fanout(...)` for explicit branch creation
- `add_join(...)` for merge-aware join points
- `add_subgraph(...)` for native subgraph composition

The executable coverage for those shapes lives in [`../../python/tests/agent_workflows_smoke.py`](../../python/tests/agent_workflows_smoke.py). That file includes:

- a single-agent loop
- a multi-agent handoff flow
- subgraph composition with namespaced stream events
- concurrent subgraph execution through `abatch(...)`
- fan-out and join with merge strategies

If you use `add_subgraph(...)`, the runtime propagates the Python `config` into child runs. That config must therefore be pickle-serializable.

## Batch And Concurrency

There are two distinct concurrency layers to keep in mind:

- `worker_count` controls native worker parallelism inside a compiled graph
- `abatch(...)` schedules multiple graph invocations concurrently from Python

Use `batch(...)` when deterministic in-process sequencing is more useful than outer concurrency. Use `abatch(...)` when you want multiple invocations active at the same time.

## Where To Look Next

- API summary: [`../reference/api.md`](../reference/api.md)
- Runtime model: [`../concepts/runtime-model.md`](../concepts/runtime-model.md)
- Validation commands: [`../operations/validation.md`](../operations/validation.md)
