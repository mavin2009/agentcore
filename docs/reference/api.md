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
- `RuntimeContext`

### `StateGraph`

Constructor:

```python
StateGraph(state_schema=None, *, name=None, worker_count=1)
```

Builder methods:

- `add_node(name, action=None, *, kind="compute", stop_after=False, allow_fan_out=False, create_join_scope=False, join_incoming_branches=False, merge=None)`
- `add_fanout(name, action=None, *, kind="control", create_join_scope=True)`
- `add_join(name, action=None, *, merge=None, kind="aggregate")`
- `add_subgraph(name, graph, *, inputs=None, outputs=None, namespace=None, propagate_knowledge_graph=False, session_mode="ephemeral", session_id_from=None)`
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

Subgraph session rules:

- `session_mode="ephemeral"` is the default and requires `session_id_from=None`
- `session_mode="persistent"` requires `session_id_from` to name a parent state field
- persistent sessions commit their child snapshot only on successful child completion
- concurrent reuse of the same `(subgraph node, session_id)` in one run is rejected

### `CompiledStateGraph`

Execution methods:

- `invoke(input_state=None, *, config=None)`
- `invoke_with_metadata(input_state=None, *, config=None, include_subgraphs=True)`
- `invoke_until_pause_with_metadata(input_state=None, *, config=None, include_subgraphs=True)`
- `resume_with_metadata(checkpoint_id, *, include_subgraphs=True)`
- `stream(input_state=None, *, config=None, include_subgraphs=True, stream_mode="events")`
- `ainvoke(input_state=None, *, config=None)`
- `ainvoke_with_metadata(input_state=None, *, config=None, include_subgraphs=True)`
- `astream(input_state=None, *, config=None, include_subgraphs=True, stream_mode="events")`
- `batch(inputs, *, config=None)`
- `abatch(inputs, *, config=None)`

Current stream mode support:

- `stream_mode="events"`

Metadata and stream events include the usual run/node identifiers plus:

- `session_id`
- `session_revision`
- `namespaces`

### `Command`

```python
Command(update=None, goto=None, wait=False)
```

Use `Command` when a node wants to return state updates together with explicit routing or a native wait result.

### `RuntimeContext`

`RuntimeContext` is passed to Python callbacks that declare a third positional argument.

Current surface:

- `runtime.available`
- `runtime.record_once(key, request, producer)`
- `runtime.record_once_with_metadata(key, request, producer)`

`record_once(...)` returns the produced value. `record_once_with_metadata(...)` returns a dictionary with:

- `value`
- `replayed`
- `flags`

The `producer` callable is invoked only when no previously committed recorded effect exists for the same key and request payload inside the active run.

### Python Node Callback Contract

A Python node callback may:

- accept `state`
- accept `(state, config)`
- accept `(state, config, runtime)`
- return a mapping of updates
- return `None`
- return a node name string
- return `Command(...)`
- return `(updates, goto)`
- return an awaitable resolving to one of the above

`wait=True` is only available through `Command(...)`.

## C++ Surface

The most important headers are:

- [`../../agentcore/include/agentcore/core/types.h`](../../agentcore/include/agentcore/core/types.h): common IDs, `Value`, blob and string handle types
- [`../../agentcore/include/agentcore/graph/graph_ir.h`](../../agentcore/include/agentcore/graph/graph_ir.h): `GraphDefinition`, node and edge definitions, graph compilation helpers
- [`../../agentcore/include/agentcore/state/state_store.h`](../../agentcore/include/agentcore/state/state_store.h): `WorkflowState`, `StatePatch`, `BlobStore`, patch log
- [`../../agentcore/include/agentcore/state/journal/task_journal.h`](../../agentcore/include/agentcore/state/journal/task_journal.h): persisted recorded-task journal
- [`../../agentcore/include/agentcore/state/knowledge_graph.h`](../../agentcore/include/agentcore/state/knowledge_graph.h): knowledge-graph storage and matching
- [`../../agentcore/include/agentcore/runtime/node_runtime.h`](../../agentcore/include/agentcore/runtime/node_runtime.h): `ExecutionContext`, `NodeResult`, node execution contract
- [`../../agentcore/include/agentcore/runtime/scheduler.h`](../../agentcore/include/agentcore/runtime/scheduler.h): scheduler, worker pool, async completion queue
- [`../../agentcore/include/agentcore/runtime/tool_api.h`](../../agentcore/include/agentcore/runtime/tool_api.h): tool request/response contracts
- [`../../agentcore/include/agentcore/runtime/model_api.h`](../../agentcore/include/agentcore/runtime/model_api.h): model request/response contracts
- [`../../agentcore/include/agentcore/execution/engine.h`](../../agentcore/include/agentcore/execution/engine.h): `ExecutionEngine`, run lifecycle, public stream access
- [`../../agentcore/include/agentcore/execution/checkpoint.h`](../../agentcore/include/agentcore/execution/checkpoint.h): checkpoints, snapshots, and pluggable storage backends
- [`../../agentcore/include/agentcore/execution/proof.h`](../../agentcore/include/agentcore/execution/proof.h): snapshot and trace digest support

### Key Native Types

- `GraphDefinition`: immutable workflow structure after compilation/setup
- `NodeDefinition`: node kind, policy flags, executor, retries, timeout, outgoing edges
- `EdgeDefinition`: routing relationship and optional condition
- `ExecutionEngine`: run lifecycle and state/tracing access
- `InterruptResult`: result of intentionally pausing a live run
- `StateEditResult`: result of applying a manual state patch to a paused run
- `ExecutionContext`: per-step view of state, blobs, registries, trace sink, deadlines, and knowledge graph
- `NodeResult`: status, patch, confidence, flags, optional next-node override
- `StatePatch`: field updates, blobs, recorded task outcomes, and knowledge-graph writes
- `TaskJournal`: persisted once-only task/outcome records scoped to a run snapshot
- `RecordedEffectResult`: return type for `ExecutionContext` recorded-effect helpers

### Tool And Model Registration

The engine exposes registries directly:

- `engine.tools().register_tool(...)`
- `engine.models().register_model(...)`

Those adapters are invoked from nodes through `ExecutionContext`.

### Recorded Synchronous Effects

The native execution context includes a small recorded-effect seam for synchronous external work that must not be re-executed after checkpoint restore.

Current helpers on `ExecutionContext`:

- `find_recorded_effect(key)`
- `record_blob_effect_once(key, request, producer, flags=0)`
- `record_text_effect_once(key, request_text, producer, flags=0)`

These helpers look up previously committed outcomes in the run-local `TaskJournal`, validate the request payload for the same key, and only invoke the producer when no prior committed outcome exists. The resulting task records are committed through the engine as part of the same state-commit path used for ordinary `StatePatch` updates.

### Operational Control Surface

The execution engine now exposes a small explicit operational seam for durable runs:

- `set_checkpointer(...)`
- `enable_checkpoint_persistence(...)`
- `enable_sqlite_checkpoint_persistence(...)`
- `load_persisted_checkpoints()`
- `interrupt(run_id)`
- `inspect(run_id)`
- `apply_state_patch(run_id, patch, branch_id)`
- `resume_run(run_id)`
- `resume(checkpoint_id)`

That split is intentional: `resume(checkpoint_id)` restores a run from persisted state, while `resume_run(run_id)` continues an already-paused live run after inspection or manual state edits.

## Related Pages

- Python quickstart: [`../quickstarts/python.md`](../quickstarts/python.md)
- C++ quickstart: [`../quickstarts/cpp.md`](../quickstarts/cpp.md)
- Runtime model: [`../concepts/runtime-model.md`](../concepts/runtime-model.md)
- Validation: [`../operations/validation.md`](../operations/validation.md)
