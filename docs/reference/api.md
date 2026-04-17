# API Map

This page is a compact reference to the public surfaces that are easiest to build against from this repository.

## Python Surface

The Python package surface is exported from [`../../python/agentcore/__init__.py`](../../python/agentcore/__init__.py) and implemented primarily in [`../../python/agentcore/graph/state.py`](../../python/agentcore/graph/state.py).

Exports:

- `StateGraph`
- `CompiledStateGraph`
- `START`
- `END`
- `Command`

### `StateGraph`

Constructor:

```python
StateGraph(state_schema=None, *, name=None, worker_count=1)
```

Builder methods:

- `add_node(name, action=None, *, kind="compute", stop_after=False, allow_fan_out=False, create_join_scope=False, join_incoming_branches=False, merge=None)`
- `add_fanout(name, action=None, *, kind="control", create_join_scope=True)`
- `add_join(name, action=None, *, merge=None, kind="aggregate")`
- `add_subgraph(name, graph, *, inputs=None, outputs=None, namespace=None, propagate_knowledge_graph=False)`
- `add_edge(source, target)`
- `add_conditional_edges(source, path, path_map=None)`
- `set_entry_point(name)`
- `compile(*, worker_count=None)`

Supported Python node kinds:

- `compute`
- `control`
- `tool`
- `model`
- `aggregate`
- `human`

Subgraphs are added through `add_subgraph(...)` rather than `add_node(..., kind="subgraph")`.

### `CompiledStateGraph`

Execution methods:

- `invoke(input_state=None, *, config=None)`
- `invoke_with_metadata(input_state=None, *, config=None, include_subgraphs=True)`
- `stream(input_state=None, *, config=None, include_subgraphs=True, stream_mode="events")`
- `ainvoke(input_state=None, *, config=None)`
- `ainvoke_with_metadata(input_state=None, *, config=None, include_subgraphs=True)`
- `astream(input_state=None, *, config=None, include_subgraphs=True, stream_mode="events")`
- `batch(inputs, *, config=None)`
- `abatch(inputs, *, config=None)`

Current stream mode support:

- `stream_mode="events"`

### `Command`

```python
Command(update=None, goto=None)
```

Use `Command` when a node wants to return both state updates and an explicit next node.

### Python Node Callback Contract

A Python node callback may:

- accept `state`
- accept `(state, config)`
- return a mapping of updates
- return `None`
- return a node name string
- return `Command(...)`
- return `(updates, goto)`
- return an awaitable resolving to one of the above

## C++ Surface

The most important headers are:

- [`../../agentcore/include/agentcore/core/types.h`](../../agentcore/include/agentcore/core/types.h): common IDs, `Value`, blob and string handle types
- [`../../agentcore/include/agentcore/graph/graph_ir.h`](../../agentcore/include/agentcore/graph/graph_ir.h): `GraphDefinition`, node and edge definitions, graph compilation helpers
- [`../../agentcore/include/agentcore/state/state_store.h`](../../agentcore/include/agentcore/state/state_store.h): `WorkflowState`, `StatePatch`, `BlobStore`, patch log
- [`../../agentcore/include/agentcore/state/knowledge_graph.h`](../../agentcore/include/agentcore/state/knowledge_graph.h): knowledge-graph storage and matching
- [`../../agentcore/include/agentcore/runtime/node_runtime.h`](../../agentcore/include/agentcore/runtime/node_runtime.h): `ExecutionContext`, `NodeResult`, node execution contract
- [`../../agentcore/include/agentcore/runtime/scheduler.h`](../../agentcore/include/agentcore/runtime/scheduler.h): scheduler, worker pool, async completion queue
- [`../../agentcore/include/agentcore/runtime/tool_api.h`](../../agentcore/include/agentcore/runtime/tool_api.h): tool request/response contracts
- [`../../agentcore/include/agentcore/runtime/model_api.h`](../../agentcore/include/agentcore/runtime/model_api.h): model request/response contracts
- [`../../agentcore/include/agentcore/execution/engine.h`](../../agentcore/include/agentcore/execution/engine.h): `ExecutionEngine`, run lifecycle, public stream access
- [`../../agentcore/include/agentcore/execution/checkpoint.h`](../../agentcore/include/agentcore/execution/checkpoint.h): checkpoints and snapshots
- [`../../agentcore/include/agentcore/execution/proof.h`](../../agentcore/include/agentcore/execution/proof.h): snapshot and trace digest support

### Key Native Types

- `GraphDefinition`: immutable workflow structure after compilation/setup
- `NodeDefinition`: node kind, policy flags, executor, retries, timeout, outgoing edges
- `EdgeDefinition`: routing relationship and optional condition
- `ExecutionEngine`: run lifecycle and state/tracing access
- `ExecutionContext`: per-step view of state, blobs, registries, trace sink, deadlines, and knowledge graph
- `NodeResult`: status, patch, confidence, flags, optional next-node override
- `StatePatch`: field updates, blobs, and knowledge-graph writes

### Tool And Model Registration

The engine exposes registries directly:

- `engine.tools().register_tool(...)`
- `engine.models().register_model(...)`

Those adapters are invoked from nodes through `ExecutionContext`.

## Related Pages

- Python quickstart: [`../quickstarts/python.md`](../quickstarts/python.md)
- C++ quickstart: [`../quickstarts/cpp.md`](../quickstarts/cpp.md)
- Runtime model: [`../concepts/runtime-model.md`](../concepts/runtime-model.md)
- Validation: [`../operations/validation.md`](../operations/validation.md)
