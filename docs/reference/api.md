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
- `MessagesState`
- `add_messages`
- `PromptTemplate`
- `MessageTemplate`
- `ChatPromptTemplate`
- `RenderedPrompt`
- `RenderedChatPrompt`
- `IntelligenceView`
- `IntelligenceRule`
- `IntelligenceRouter`
- `IntelligenceSubscription`
- `RuntimeContext`
- `ToolRegistryView`
- `ModelRegistryView`
- `PipelineGraph`
- `PipelineStep`
- `SpecialistTeam`
- `Specialist`

### `StateGraph`

Constructor:

```python
StateGraph(state_schema=None, *, name=None, worker_count=1)
```

Builder methods:

- `add_node(name_or_action, action=None, *, kind="compute", stop_after=False, allow_fan_out=False, create_join_scope=False, join_incoming_branches=False, deterministic=False, read_keys=None, cache_size=16, intelligence_subscriptions=None, merge=None)`
- `add_fanout(name, action=None, *, kind="control", create_join_scope=True)`
- `add_join(name, action=None, *, merge=None, kind="aggregate")`
- `add_subgraph(name, graph, *, inputs=None, outputs=None, namespace=None, propagate_knowledge_graph=False, session_mode="ephemeral", session_id_from=None)`
- `add_edge(source, target)`
- `add_conditional_edges(source, path, path_map=None)`
- `add_sequence(nodes)`
- `set_entry_point(name)`
- `set_finish_point(name)`
- `set_conditional_entry_point(path, path_map=None)`
- `compile(*, worker_count=None, name=None, debug=None, checkpointer=None, interrupt_before=None, interrupt_after=None, store=None)`

Supported Python node kinds:

- `compute`
- `control`
- `tool`
- `model`
- `aggregate`
- `human`

Subgraphs are added through `add_subgraph(...)` rather than `add_node(..., kind="subgraph")`.

Migration-friendly builder notes:

- `add_node(...)` accepts either `(name, action)` or a named callable by itself. Lambdas and anonymous callables still require an explicit name.
- `add_node(name, graph)` and `add_node(graph)` also accept a `StateGraph` or `CompiledStateGraph` directly when the parent and child declare overlapping schema fields. In that case the shared fields are bound by name for both input and output, which gives a compact shared-state subgraph path without writing bindings by hand.
- `add_sequence([...])` accepts registered node names, named callables, or `(name, action)` pairs and wires them in order.
- `add_edge([...], target)` treats a sequence source as a multi-source join edge and marks the target as a join node automatically.
- `add_conditional_edges(START, ...)` is accepted as an alias for `set_conditional_entry_point(...)`.

Schema-driven reducer notes:

- If `state_schema` is a declared schema with `Annotated[...]` fields, a supported reducer subset is inferred automatically for join barriers.
- `Annotated[int, operator.add]` maps to `sum_int64`.
- `Annotated[int, min]` maps to `min_int64`.
- `Annotated[int, max]` maps to `max_int64`.
- `Annotated[list[T], operator.add]` maps to `concat_sequence`.
- `Annotated[list[dict], add_messages]` maps to `merge_messages`.
- `Annotated[bool, operator.or_]` maps to `logical_or`.
- `Annotated[bool, operator.and_]` maps to `logical_and`.
- List concatenation is implemented as a native tagged sequence merge. Python list values are stored as sequence blobs, concatenated in deterministic join order, and decoded back to Python lists.
- Message merging is implemented as a native tagged message merge. Messages with a non-empty `id` replace the existing message with that id while preserving that message's position; messages without an id append.
- Reducers outside that subset are not inferred automatically today. Use explicit `merge={...}` rules or a post-join aggregation node when you need a merge shape the native join engine does not currently expose.

Message state helpers:

```python
from agentcore.graph import MessagesState, add_messages


class AgentState(MessagesState, total=False):
    summary: str
```

`MessagesState` declares a `messages` field using `Annotated[list[dict], add_messages]`. The standalone `add_messages(left, right)` helper has the same append-or-replace-by-id behavior in Python, and the schema metadata compiles to the native `merge_messages` strategy at join barriers.

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

`invoke_with_metadata(...)`, `invoke_until_pause_with_metadata(...)`, and `resume_with_metadata(...)` also include the committed intelligence snapshot under `details["intelligence"]`.

`compile(...)` compatibility notes:

- `name=` overrides the compiled graph name for that compiled instance.
- `debug=` is accepted for migration convenience and does not change runtime behavior.
- `checkpointer=`, `interrupt_before=`, `interrupt_after=`, and `store=` currently raise a clear `NotImplementedError` so migration failures stay explicit instead of silently ignoring LangGraph-specific durability semantics.
- Direct `add_node(subgraph)` is intentionally limited to the shared-schema default path. Use `add_subgraph(...)` when you need namespaces, persistent sessions, knowledge-graph propagation, or explicit field bindings.

Registry properties:

- `compiled.tools`
- `compiled.models`

### `agentcore.patterns`

The optional higher-level builder layer lives under [`../../python/agentcore/patterns`](../../python/agentcore/patterns). These helpers still compile into the same native-backed `StateGraph` runtime.

#### `PipelineGraph`

Constructor:

```python
PipelineGraph(state_schema=None, *, name=None, worker_count=1)
```

Methods:

- `add_step(name, action=None, *, kind="compute", stop_after=False, allow_fan_out=False, create_join_scope=False, join_incoming_branches=False, merge=None)`
- `extend(steps)`
- `build()`
- `compile(*, worker_count=None)`

`PipelineGraph` is the higher-level surface for strictly sequential stage workflows. It removes the repetitive `START -> step1 -> step2 -> ... -> END` wiring while preserving the same native execution path underneath.

#### `SpecialistTeam`

Related type:

- `Specialist`

Constructor:

```python
SpecialistTeam(state_schema=None, *, name=None, worker_count=4)
```

Methods:

- `set_dispatch(name="dispatch", action=None, *, kind="control")`
- `set_aggregate(name="aggregate", action=None, *, merge=None, kind="aggregate")`
- `add_specialist(specialist, graph=None, *, inputs=None, outputs=None, namespace=None, propagate_knowledge_graph=False, session_mode="persistent", session_id_from=None, prepare=None, prepare_kind="compute")`
- `build()`
- `compile(*, worker_count=None)`

`SpecialistTeam` lowers a common multi-agent shape into a dispatch fan-out, optional per-specialist preparation, subgraph invocation, and aggregate/join node. Persistent specialists can infer `session_id_from` automatically when exactly one input binding maps a parent field to child `session_id`.

### `agentcore.prompts`

These prompt helpers live under [`../../python/agentcore/prompts`](../../python/agentcore/prompts) and are also exported from `agentcore` directly.

#### `PromptTemplate`

```python
PromptTemplate(template, *, defaults=None, name=None)
```

Current surface:

- `.variables`
- `.partial(**defaults)`
- `.render(values=None, **kwargs)`
- `.format(values=None, **kwargs)`
- `.to_model_input()`

`PromptTemplate` renders to `RenderedPrompt`, which stringifies mappings and sequences in a stable JSON-friendly form when no explicit format specifier is provided.

#### `MessageTemplate`

```python
MessageTemplate(role, template, *, defaults=None, name=None)
```

Current surface:

- `.variables`
- `.partial(**defaults)`
- `.render(values=None, **kwargs)`
- `.format(values=None, **kwargs)`

`MessageTemplate.render(...)` returns a `PromptMessage`.

#### `ChatPromptTemplate`

```python
ChatPromptTemplate.from_messages(messages, *, name=None, separator="\n\n")
```

Accepted message forms:

- `MessageTemplate(...)`
- `PromptMessage(...)`
- `(role, template)` pairs
- mappings with `role` plus `template` or `content`

Current surface:

- `.variables`
- `.partial(**defaults)`
- `.render(values=None, **kwargs)`
- `.format(values=None, **kwargs)`
- `.as_messages(values=None, **kwargs)`
- `.to_model_input(mode="text" | "messages")`

`render(...)` returns `RenderedChatPrompt`.

#### `RenderedPrompt`

```python
RenderedPrompt(text, *, name=None)
```

Current surface:

- `.text`
- `.to_model_input()`

`RenderedPrompt` stringifies to its rendered text and can be passed directly to model invocation surfaces.

#### `RenderedChatPrompt`

```python
RenderedChatPrompt(messages, *, name=None, separator="\n\n")
```

Current surface:

- `.messages`
- `.as_messages()`
- `.to_text(include_roles=True, separator=None)`
- `.to_model_input(mode="text" | "messages")`

Built-in native chat adapters currently consume text prompt payloads, so `mode="text"` is the default. `mode="messages"` returns a JSON-friendly message list for custom Python-backed model handlers that register with `decode_prompt="json"`.

### `Command`

```python
Command(update=None, goto=None, wait=False)
```

Use `Command` when a node wants to return state updates together with explicit routing or a native wait result.

### `RuntimeContext`

`RuntimeContext` is passed to Python callbacks that declare a third positional argument.

Current surface:

- `runtime.available`
- `runtime.intelligence`
- `runtime.snapshot_intelligence()`
- `runtime.upsert_task(key, *, title=None, owner=None, details=None, result=None, status=None, priority=None, confidence=None)`
- `runtime.upsert_claim(key, *, subject=None, relation=None, object=None, statement=None, status=None, confidence=None)`
- `runtime.add_evidence(key, *, kind=None, source=None, content=None, task_key=None, claim_key=None, confidence=None)`
- `runtime.record_decision(key, *, task_key=None, claim_key=None, summary=None, status=None, confidence=None)`
- `runtime.remember(key, *, layer=None, scope=None, content=None, task_key=None, claim_key=None, importance=None)`
- `runtime.record_once(key, request, producer)`
- `runtime.record_once_with_metadata(key, request, producer)`
- `runtime.invoke_tool(name, request, decode="auto")`
- `runtime.invoke_tool_with_metadata(name, request, decode="auto")`
- `runtime.invoke_model(name, prompt, schema=None, max_tokens=0, decode="auto")`
- `runtime.invoke_model_with_metadata(name, prompt, schema=None, max_tokens=0, decode="auto")`

`prompt` may be a plain Python value, `PromptTemplate`, `ChatPromptTemplate`, `RenderedPrompt`, or `RenderedChatPrompt`. Prompt objects are normalized before the request crosses into the native registry.

`runtime.intelligence` is the preferred grouped surface for intelligence operations. Current methods:

- `runtime.intelligence.snapshot()`
- `runtime.intelligence.summary()`
- `runtime.intelligence.count(kind=None, key=None, key_prefix=None, task_key=None, claim_key=None, subject=None, relation=None, object=None, owner=None, source=None, scope=None, status=None, layer=None, min_confidence=None, min_importance=None)`
- `runtime.intelligence.exists(**filters)`
- `runtime.intelligence.query(kind=None, key=None, key_prefix=None, task_key=None, claim_key=None, subject=None, relation=None, object=None, owner=None, source=None, scope=None, status=None, layer=None, min_confidence=None, min_importance=None, limit=None)`
- `runtime.intelligence.supporting_claims(key=None, key_prefix=None, task_key=None, claim_key=None, subject=None, relation=None, object=None, status=None, min_confidence=None, limit=None)`
- `runtime.intelligence.action_candidates(task_key=None, claim_key=None, subject=None, relation=None, object=None, owner=None, task_status=None, source=None, scope=None, layer=None, min_confidence=None, min_importance=None, limit=None)`
- `runtime.intelligence.related(task_key=None, claim_key=None, limit=None, hops=1)`
- `runtime.intelligence.agenda(key=None, key_prefix=None, task_key=None, owner=None, status=None, min_confidence=None, limit=None)`
- `runtime.intelligence.next_task(key=None, key_prefix=None, task_key=None, owner=None, status=None, min_confidence=None)`
- `runtime.intelligence.recall(key=None, key_prefix=None, task_key=None, claim_key=None, scope=None, layer=None, min_importance=None, limit=None)`
- `runtime.intelligence.focus(key=None, key_prefix=None, task_key=None, claim_key=None, subject=None, relation=None, object=None, owner=None, source=None, scope=None, layer=None, min_confidence=None, min_importance=None, limit=5)`
- `runtime.intelligence.route(rules, *, default=None)`
- `runtime.intelligence.upsert_task(...)`
- `runtime.intelligence.upsert_claim(...)`
- `runtime.intelligence.add_evidence(...)`
- `runtime.intelligence.record_decision(...)`
- `runtime.intelligence.remember(...)`

Routing helpers:

- `IntelligenceRule(goto, *, kind=None, key=None, key_prefix=None, task_key=None, claim_key=None, subject=None, relation=None, object=None, owner=None, source=None, scope=None, status=None, layer=None, min_confidence=None, min_importance=None, min_count=1, max_count=None)`
- `IntelligenceRouter(rules, default=None)`
- `IntelligenceSubscription(kind=None, key=None, key_prefix=None, task_key=None, claim_key=None, subject=None, relation=None, object=None, owner=None, source=None, scope=None, status=None, layer=None, min_confidence=None, min_importance=None)`

`IntelligenceRouter` is a small callable wrapper intended for `StateGraph.add_conditional_edges(...)`. It evaluates the provided `IntelligenceRule` list against the runtime intelligence view and returns the selected route name.

`IntelligenceSubscription` is used at graph-build time. Passing `intelligence_subscriptions=[...]` to `StateGraph.add_node(...)` marks that node as intelligence-reactive, and matching committed intelligence deltas will enqueue the node through the native reactive frontier scheduler.

The direct `RuntimeContext` intelligence methods remain available as compatibility wrappers around the grouped view.

`agenda(...)` returns a tasks-only snapshot ranked by task status class, descending priority, descending confidence, then stable id. `next_task(...)` is the convenience form that returns the first ranked task or `None`.

`supporting_claims(...)` returns a claims-only snapshot ranked by deterministic support from linked evidence, decisions, and memories. When `task_key=` is present, task-aligned support is prioritized so workflows can ask for the best-supported claims for one task without reimplementing the reduction path in Python.

`action_candidates(...)` is the task-selection primitive that sits between those two. It accepts task, claim, semantic-claim, evidence-source, and memory-scope anchors, expands only the bounded linked neighborhood around those anchors, and returns tasks ranked by direct anchor match, aligned support from linked evidence/decisions/memories, task status class, total support, priority, confidence, then stable id. The result shape stays tasks-only so Python nodes can ask "what should we do next?" without materializing a larger cross-kind focus set first.

`recall(...)` returns a memories-only snapshot ranked by descending importance, then stable id.

`focus(...)` returns a bounded cross-kind snapshot. It seeds from direct anchors and the current agenda/recall surface, expands one bounded related set through task/claim links, and returns ranked `tasks`, `claims`, `evidence`, `decisions`, and `memories` lists in one response. Direct anchors stay first, and decisive support such as selected decisions and strong evidence is ranked ahead of large numbers of weak links.

`related(...)` returns the neighborhood around one task or claim. With the default `hops=1`, it behaves like a first-hop expansion. Larger hop counts keep expanding through linked task/claim records in bounded BFS order so workflows can cross a shared claim or task boundary without writing custom traversal logic in Python.

For claim records, `count(...)`, `query(...)`, `focus(...)`, `IntelligenceRule`, and `IntelligenceSubscription` all support semantic filtering by `subject`, `relation`, and `object`. That allows claim-centric routing and reactive execution to stay on the same native query path used for direct lookups.

The intelligence snapshot shape is:

- `counts`
- `tasks`
- `claims`
- `evidence`
- `decisions`
- `memories`

Each list entry carries a stable numeric `id`, a string `key`, and the currently implemented typed fields for that record category. Payload-bearing fields such as task `details`, evidence `content`, decision `summary`, and memory `content` round-trip through the native blob store and return as ordinary Python objects.

`record_once(...)` returns the produced value. `record_once_with_metadata(...)` returns a dictionary with:

- `value`
- `replayed`
- `flags`

The `producer` callable is invoked only when no previously committed recorded effect exists for the same key and request payload inside the active run.

### `ToolRegistryView` And `ModelRegistryView`

`CompiledStateGraph` exposes graph-owned native registries through `.tools` and `.models`.

`ToolRegistryView` methods:

- `list()`
- `describe(name)`
- `register(name, handler, *, policy=None, metadata=None, decode_input="auto")`
- `register_http(name="http_tool", *, policy=None, enable_mock_scheme=True, enable_file_scheme=True)`
- `register_sqlite(name="sqlite_tool", *, policy=None)`
- `register_http_json(name="http_json_tool", *, policy=None, transport=None, default_method="POST")`
- `invoke(name, request, *, decode="auto")`
- `invoke_with_metadata(name, request, *, decode="auto")`

`ModelRegistryView` methods:

- `list()`
- `describe(name)`
- `register(name, handler, *, policy=None, metadata=None, decode_prompt="auto", decode_schema="auto")`
- `register_local(name="local_model", *, policy=None, default_max_tokens=256)`
- `register_llm_http(name="llm_http", *, policy=None, enable_mock_transport=True)`
- `register_openai_chat(name="openai_chat", *, policy=None, transport=None, provider_model_name="", endpoint_path="/chat/completions", system_prompt="", include_json_schema=True)`
- `register_grok_chat(name="grok_chat", *, policy=None, transport=None, provider_model_name="", endpoint_path="/chat/completions", system_prompt="", include_json_schema=True)`
- `register_gemini_generate_content(name="gemini", *, policy=None, transport=None, provider_model_name="", endpoint_path="", system_prompt="")`
- `invoke(name, prompt, *, schema=None, max_tokens=0, decode="auto")`
- `invoke_with_metadata(name, prompt, *, schema=None, max_tokens=0, decode="auto")`

`prompt` accepts the same rendered prompt objects supported by `RuntimeContext.invoke_model(...)`. `schema` may also be a prompt object if you want to generate a text or JSON schema payload through the same rendering surface.

Invocation details currently include:

- `ok`
- `output`
- `flags`
- `attempts`
- `latency_ns`
- `error_category`

Model invocation details also include:

- `confidence`
- `token_usage`

Custom Python-backed registry handlers are still registered into the native graph-owned registries rather than a Python-only side table. That keeps direct invocation, runtime invocation through `RuntimeContext`, adapter discovery, and subgraph inheritance on the same native registry path.

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
- [`../../agentcore/include/agentcore/state/intelligence/model.h`](../../agentcore/include/agentcore/state/intelligence/model.h): intelligence records, patches, deltas, and snapshots
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
- `ExecutionContext`: per-step view of state, blobs, registries, trace sink, deadlines, knowledge graph, and intelligence state
- `IntelligenceStore`: structured task/claim/evidence/decision/memory state owned by `StateStore`
- `NodeResult`: status, patch, confidence, flags, optional next-node override
- `StatePatch`: field updates, blobs, recorded task outcomes, intelligence writes, and knowledge-graph writes
- `TaskJournal`: persisted once-only task/outcome records scoped to a run snapshot
- `RecordedEffectResult`: return type for `ExecutionContext` recorded-effect helpers

### Tool And Model Registration

The engine exposes registries directly:

- `engine.tools().register_tool(...)`
- `engine.models().register_model(...)`
- `engine.tools().describe_tool(name)`
- `engine.models().describe_model(name)`

Those adapters are invoked from nodes through `ExecutionContext`.

Current registry specs also carry adapter metadata through [`../../agentcore/include/agentcore/adapters/common/adapter_metadata.h`](../../agentcore/include/agentcore/adapters/common/adapter_metadata.h).

Provider-style HTTP adapters also build on the shared transport seam in [`../../agentcore/include/agentcore/adapters/common/http_transport.h`](../../agentcore/include/agentcore/adapters/common/http_transport.h), which defines:

- `HttpRequest`
- `HttpResponse`
- `HttpTransport`
- `HttpTransportOptions`

That seam is used by the built-in HTTP JSON tool adapter, the OpenAI-compatible chat adapter, the xAI Grok adapter, and the Gemini adapter so auth, timeout, and header handling stay shared where protocols overlap instead of being reimplemented inside each provider module. Gemini still has its own direct request builder because its `generateContent` API shape is meaningfully different from chat-completions style providers.

### Adapter Metadata And Capabilities

`AdapterMetadata` currently describes:

- `provider`
- `implementation`
- `display_name`
- `transport`
- `auth`
- `capabilities`
- `request_format`
- `response_format`

Capability flags currently include:

- `kAdapterCapabilitySync`
- `kAdapterCapabilityAsync`
- `kAdapterCapabilityStreaming`
- `kAdapterCapabilityStructuredRequest`
- `kAdapterCapabilityStructuredResponse`
- `kAdapterCapabilityCheckpointSafe`
- `kAdapterCapabilityExternalNetwork`
- `kAdapterCapabilityLocalFilesystem`
- `kAdapterCapabilityJsonSchema`
- `kAdapterCapabilityToolCalling`
- `kAdapterCapabilitySql`
- `kAdapterCapabilityChatMessages`

Helpers:

- `adapter_has_capability(metadata, capability)`
- `adapter_transport_name(...)`
- `adapter_auth_name(...)`

The built-in adapters now register metadata by default, so callers can inspect registry contents without relying on adapter-specific naming conventions.

### Stable Error Categorization

Tool/model handlers still return flag-based results, but the public runtime surface now includes stable category helpers:

- `classify_tool_response_flags(flags)`
- `tool_error_category_name(category)`
- `classify_model_response_flags(flags)`
- `model_error_category_name(category)`

The current categories are:

- `missing_handler`
- `validation`
- `limits`
- `timeout`
- `unsupported`
- `handler_exception`
- `retry_exhausted`
- `none`

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
