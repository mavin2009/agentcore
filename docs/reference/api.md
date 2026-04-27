# API Map

This page is a compact reference to the public surfaces that are easiest to build against from this repository.

Use this page after you have skimmed the [Python quickstart](../quickstarts/python.md) or [runtime model](../concepts/runtime-model.md). It is organized as a map of implemented surfaces rather than a tutorial, so it is best for answering "what is the exact method name or argument?" while you are writing code.

Common lookups:

- graph construction: [`StateGraph`](#stategraph)
- graph execution: [`CompiledStateGraph`](#compiledstategraph)
- context assembly: [`ContextSpec`](#contextspec) and [`ContextView`](#contextview)
- telemetry: [`OpenTelemetryObserver`](#opentelemetryobserver)
- prompts: [`agentcore.prompts`](#agentcoreprompts)
- runtime helper methods inside node callbacks: [`RuntimeContext`](#runtimecontext)
- MCP client/server support: [`agentcore.mcp`](#agentcoremcp)
- native embedding: [C++ Surface](#c-surface)

## Python Surface

The Python package surface is exported from [`../../python/agentcore/__init__.py`](../../python/agentcore/__init__.py) and implemented primarily in [`../../python/agentcore/graph/state.py`](../../python/agentcore/graph/state.py).

Exports:

- `StateGraph`
- `CompiledStateGraph`
- `ContextSpec`
- `ContextView`
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
- `RenderedMCPPrompt`
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
- `mcp`
- `observability`
- `OpenTelemetryObserver`

### `StateGraph`

Constructor:

```python
StateGraph(state_schema=None, *, name=None, worker_count=1)
```

Builder methods:

- `add_node(name_or_action, action=None, *, kind="compute", stop_after=False, allow_fan_out=False, create_join_scope=False, join_incoming_branches=False, deterministic=False, read_keys=None, cache_size=16, intelligence_subscriptions=None, context=None, merge=None)`
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
- `invoke_with_metadata(input_state=None, *, config=None, include_subgraphs=True, telemetry=None)`
- `invoke_until_pause_with_metadata(input_state=None, *, config=None, include_subgraphs=True, telemetry=None)`
- `resume_with_metadata(checkpoint_id, *, include_subgraphs=True, telemetry=None)`
- `stream(input_state=None, *, config=None, include_subgraphs=True, stream_mode="events", telemetry=None)`
- `ainvoke(input_state=None, *, config=None, telemetry=None)`
- `ainvoke_with_metadata(input_state=None, *, config=None, include_subgraphs=True, telemetry=None)`
- `astream(input_state=None, *, config=None, include_subgraphs=True, stream_mode="events", telemetry=None)`
- `batch(inputs, *, config=None, telemetry=None)`
- `abatch(inputs, *, config=None, telemetry=None)`

The same `telemetry=` keyword is also accepted by `invoke(...)`:

- `invoke(input_state=None, *, config=None, telemetry=None)`

Current stream mode support:

- `stream_mode="events"`

Metadata and stream events include the usual run/node identifiers plus:

- `sequence`
- `graph_id`
- `node_id`
- `branch_id`
- `checkpoint_id`
- `node_kind`
- `result`
- `ts_start_ns`
- `ts_end_ns`
- `duration_ns`
- `confidence`
- `patch_count`
- `flags`
- `session_id`
- `session_revision`
- `namespaces`

`invoke_with_metadata(...)`, `invoke_until_pause_with_metadata(...)`, and `resume_with_metadata(...)` also include the committed intelligence snapshot under `details["intelligence"]`.

OpenTelemetry notes:

- `telemetry=True` constructs a default `OpenTelemetryObserver()`
- passing an explicit `OpenTelemetryObserver(...)` lets you control tracer/meter injection, node-span emission, and metric emission
- without `telemetry=...`, these methods stay on their usual execution path

### `agentcore.observability`

The observability helpers live under [`../../python/agentcore/observability`](../../python/agentcore/observability).

#### `OpenTelemetryObserver`

```python
OpenTelemetryObserver(
    *,
    tracer=None,
    meter=None,
    tracer_provider=None,
    meter_provider=None,
    instrumentation_scope="agentcore",
    instrumentation_version=None,
    run_span_name_prefix="agentcore",
    emit_node_spans=True,
    emit_metrics=True,
)
```

Current observer entry points:

- `capture_details(call, *, graph_name, operation, config=None, include_subgraphs=None)`
- `capture_stream(call, *, graph_name, operation, config=None, include_subgraphs=None)`

In normal application code you usually do not call those methods directly. Instead, pass the observer through the compiled graph methods:

```python
observer = OpenTelemetryObserver()
details = compiled.invoke_with_metadata({"count": 0}, telemetry=observer)
```

`compile(...)` compatibility notes:

- `name=` overrides the compiled graph name for that compiled instance.
- `debug=` is accepted for migration convenience and does not change runtime behavior.
- `checkpointer=`, `interrupt_before=`, `interrupt_after=`, and `store=` currently raise a clear `NotImplementedError` so migration failures stay explicit instead of silently ignoring LangGraph-specific durability semantics.
- Direct `add_node(subgraph)` is intentionally limited to the shared-schema default path. Use `add_subgraph(...)` when you need namespaces, persistent sessions, knowledge-graph propagation, or explicit field bindings.

Registry properties:

- `compiled.tools`
- `compiled.models`
- `compiled.graph_stores`

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

#### `RenderedMCPPrompt`

```python
RenderedMCPPrompt(messages, *, name=None, description=None, separator="\n\n")
```

Current surface:

- `.messages`
- `.name`
- `.description`
- `.as_messages()`
- `.to_text(include_roles=True, separator=None)`
- `.to_model_input(mode="text" | "messages" | "protocol")`

`RenderedMCPPrompt` is returned by `agentcore.mcp.StdioMCPClient.get_prompt(...)`. `mode="text"` flattens the MCP prompt to a role-prefixed text transcript, inlining resource text when available. `mode="messages"` and `mode="protocol"` return message dictionaries with MCP-style content blocks.

### `Command`

```python
Command(update=None, goto=None, wait=False)
```

Use `Command` when a node wants to return state updates together with explicit routing or a native wait result.

### `ContextSpec`

```python
ContextSpec(
    goal_key=None,
    include=None,
    budget_tokens=None,
    budget_items=32,
    require_citations=False,
    freshness="staged",
    message_key="messages",
    limit_per_source=5,
    task_key=None,
    claim_key=None,
    owner=None,
    scope=None,
    subject=None,
    relation=None,
    object=None,
    source=None,
)
```

`ContextSpec` declares how a node should assemble prompt-ready context. Pass it to `StateGraph.add_node(..., context=ContextSpec(...))`, then call `runtime.context.view()` inside the node.

The public API stays intentionally small. For intelligence and native knowledge selectors, the runtime uses an internal native context-graph query plan and returns the same `ContextView` shape; there is no separate user-facing context-index object to configure. Python fallback logic is kept for compatibility and for selector types that are inherently Python-state based.

Supported selectors include:

- `messages.recent` and `messages.all`
- `tasks.agenda`
- `claims.supported`, `claims.confirmed`, and `claims.all`
- `evidence.relevant` and `evidence.all`
- `decisions.selected` and `decisions.all`
- `memories.working`, `memories.episodic`, `memories.semantic`, `memories.procedural`, and `memories.recall`
- `actions.candidates`
- `intelligence.focus`
- `state.<field_name>`
- `knowledge.neighborhood`, which reads native `runtime.knowledge` triples first and then falls back to graph-shaped state under `knowledge_graph`, `knowledge`, or `triples`

### `ContextView`

`runtime.context.view()` returns a `ContextView`.

Current surface:

- `.spec`
- `.goal`
- `.items`
- `.provenance`
- `.conflicts`
- `.budget`
- `.digest`
- `.to_dict()`
- `.to_prompt(system=None)`
- `.to_messages(system=None)`
- `.to_model_input(mode="text" | "messages" | "dict" | "protocol", system=None)`

`invoke_with_metadata(...)`, pause/resume metadata calls, and `stream(...)` attach context metadata when a node creates a context view. The run details include `details["context"]`; matching trace events include `context_views` and `context_digest`; `details["proof"]["context_digest"]` carries the Python-level context-view digest.

Context item ordering is deterministic for a given state, `ContextSpec`, and runtime version. Intelligence and knowledge candidates are ranked natively, then message/state items are merged without changing the returned object model.

### `RuntimeContext`

`RuntimeContext` is passed to Python callbacks that declare a third positional argument.

Current surface:

- `runtime.available`
- `runtime.intelligence`
- `runtime.knowledge`
- `runtime.graph_stores`
- `runtime.context`
- `runtime.snapshot_intelligence()`
- `runtime.upsert_knowledge_entity(label, *, payload=None)`
- `runtime.upsert_knowledge_triple(subject, relation, object, *, payload=None)`
- `runtime.query_knowledge(subject=None, relation=None, object=None, direction="match", limit=None)`
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

`prompt` may be a plain Python value, `PromptTemplate`, `ChatPromptTemplate`, `RenderedPrompt`, `RenderedChatPrompt`, or `RenderedMCPPrompt`. Prompt objects are normalized before the request crosses into the native registry.

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

`runtime.knowledge` is the grouped surface for native knowledge-graph operations available inside callbacks. Current methods:

- `runtime.knowledge.upsert_entity(label, *, payload=None)`
- `runtime.knowledge.upsert_triple(subject, relation, object, *, payload=None)`
- `runtime.knowledge.query(subject=None, relation=None, object=None, direction="match", limit=None)`
- `runtime.knowledge.neighborhood(entity, *, relation=None, limit=None)`
- `runtime.knowledge.load_query(store, subject=None, relation=None, object=None, direction="match", depth=1, limit=None, properties=None)`
- `runtime.knowledge.load_neighborhood(entity=None, *, store, subject=None, relation=None, depth=1, limit=None, properties=None)`
- `runtime.knowledge.sync_to_store(store, *, subject=None, relation=None, object=None, direction="match", limit=None)`

`query(...)` returns `{"triples": [...], "counts": {"entities": n, "triples": n}, "staged": bool}`. `direction="match"` applies exact filters, `direction="incoming"` treats `subject` as the target entity when no `object` is supplied, and `direction="neighborhood"`/`"both"` returns outgoing plus incoming triples around `subject`.

`load_query(...)` and `load_neighborhood(...)` read from a registered external graph store, stage the returned entities/triples into native runtime knowledge, and return a dictionary with `entities`, `triples`, optional `store`, optional `subject`, and optional `metadata`. `sync_to_store(...)` queries native runtime knowledge and writes the selected triples into the named external store.

`runtime.graph_stores` is the same graph-owned registry exposed as `compiled.graph_stores`, bound into the callback for convenient access to metadata or custom store methods.

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

### `ToolRegistryView`, `ModelRegistryView`, And `GraphStoreRegistryView`

`CompiledStateGraph` exposes graph-owned native registries through `.tools`, `.models`, and `.graph_stores`.

`ToolRegistryView` methods:

- `list()`
- `describe(name)`
- `register(name, handler, *, policy=None, metadata=None, decode_input="auto")`
- `register_http(name="http_tool", *, policy=None, enable_mock_scheme=True, enable_file_scheme=True)`
- `register_sqlite(name="sqlite_tool", *, policy=None)`
- `register_http_json(name="http_json_tool", *, policy=None, transport=None, default_method="POST")`
- `register_mcp_stdio(command, *, prefix=None, include=None, exclude=None, env=None, cwd=None, startup_timeout=10.0, request_timeout=30.0, tool_timeout=None, result_mode="auto", argument_key=None)`
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

`register_mcp_stdio(...)` mirrors tools from an external MCP server over `stdio` into the graph-owned tool registry. Imported tools are normal AgentCore tools after registration, so graph nodes still call them through `runtime.invoke_tool(...)`.

`GraphStoreRegistryView` methods:

- `list()`
- `get(name)`
- `register(name, store)`
- `register_memory(name="memory", *, entities=None, triples=None)`
- `register_neo4j(name="neo4j", *, uri=None, auth=None, database=None, driver=None, from_env=False)`
- `remove(name, *, close=False)`
- `close_all()`

Graph-store value types live under `agentcore.graphstores`:

- `GraphEntity(label, payload=None, properties=None)`
- `GraphTriple(subject, relation, object, payload=None, properties=None)`
- `GraphNeighborhood(entities=(), triples=(), store=None, subject=None, metadata=None)`
- `GraphQuery(subject=None, relation=None, object=None, direction="match", depth=1, limit=None, properties=None)`
- `GraphStore`
- `InMemoryGraphStore`
- `Neo4jGraphStore`

`InMemoryGraphStore` is deterministic and useful for tests. `Neo4jGraphStore` requires the optional `neo4j` Python driver, available through `pip install "agentcore-graph[neo4j]"`. The Neo4j adapter uses a generic `AgentCoreEntity` node label and generic `AGENTCORE_RELATION` relationship type, storing the domain relation name as data. That keeps arbitrary relation names parameterized instead of interpolated into Cypher.

### `agentcore.mcp`

The MCP bridge lives under `agentcore.mcp`.

Exports:

- `StdioMCPClient`
- `MCPServer`
- `MCPServerSession`
- `MCPToolServer`
- `MCPTool`
- `MCPPrompt`
- `MCPResource`
- `MCPResourceTemplate`
- `MCPTransportError`
- `MCPProtocolError`
- `MCP_LOG_LEVELS`
- `MCP_PROTOCOL_VERSION`
- `coerce_mcp_prompt_result(...)`
- `coerce_mcp_resource_result(...)`
- `coerce_mcp_tool_result(...)`
- `completion_values(...)`
- `config_main(...)`
- `load_target_object(...)`
- `log_level_enabled(...)`
- `normalize_elicitation_result(...)`
- `normalize_completion_result(...)`
- `normalize_log_level(...)`
- `normalize_mcp_root(...)`
- `normalize_mcp_roots(...)`
- `normalize_mcp_resource_result(...)`
- `normalize_mcp_tool_result(...)`
- `normalize_sampling_request(...)`
- `normalize_sampling_result(...)`
- `render_client_config(...)`
- `resolve_server_target(...)`
- `run_stdio_server(...)`
- `sampling_result_to_text(...)`
- `serve_main(...)`

#### `StdioMCPClient`

```python
StdioMCPClient(
    command,
    *,
    cwd=None,
    env=None,
    startup_timeout=10.0,
    request_timeout=30.0,
    client_name="agentcore",
    client_version="0.1",
    roots=None,
    roots_list_changed=True,
    sampling_handler=None,
    elicitation_handler=None,
    log_handler=None,
    notification_handler=None,
)
```

Current surface:

- `.start()`
- `.list_tools()`
- `.call_tool(name, arguments=None, *, timeout=None, result_mode="auto")`
- `.call_tool_raw(name, arguments=None, *, timeout=None)`
- `.list_prompts()`
- `.get_prompt(name, arguments=None, *, timeout=None)`
- `.get_prompt_raw(name, arguments=None, *, timeout=None)`
- `.list_resources()`
- `.list_resource_templates()`
- `.read_resource(uri, *, timeout=None, decode="auto")`
- `.read_resource_raw(uri, *, timeout=None)`
- `.complete(ref, argument_name, value, *, arguments=None, timeout=None)`
- `.complete_prompt_argument(prompt_name, argument_name, value, *, arguments=None, timeout=None)`
- `.complete_resource_argument(uri_template, argument_name, value, *, arguments=None, timeout=None)`
- `.list_roots()`
- `.set_roots(roots, *, notify=True)`
- `.set_sampling_handler(handler)`
- `.set_elicitation_handler(handler)`
- `.set_log_handler(handler)`
- `.set_notification_handler(handler)`
- `.subscribe_resource(uri, *, timeout=None)`
- `.unsubscribe_resource(uri, *, timeout=None)`
- `.set_logging_level(level, *, timeout=None)`
- `.drain_notifications()`
- `.drain_logs()`
- `.drain_resource_updates()`
- `.close()`
- `.server_info`
- `.server_capabilities`
- `.protocol_version`
- `.list_change_counts`
- `.resource_subscriptions`

`call_tool(..., result_mode="auto")` normalizes successful structured/text results for convenience and returns the raw MCP result envelope when the upstream tool reports `isError=true`.

`get_prompt(...)` returns `RenderedMCPPrompt`.

If optional client capabilities are configured before `start()`, the client advertises them during MCP `initialize`.

#### Installed MCP Launcher

The published package installs:

- `agentcore-mcp`
- `agentcore-mcp-server`
- `agentcore-mcp-config`

The module entrypoint is also available as:

```bash
python -m agentcore.mcp serve --target package.module:build_server
python -m agentcore.mcp config codex --name local-agentcore --target package.module:build_server
```

Launcher helpers exported from `agentcore.mcp`:

- `load_target_object(target)`
- `resolve_server_target(target, *, name=None, version=None, instructions=None)`
- `run_stdio_server(target, *, name=None, version=None, instructions=None)`
- `render_client_config(client, *, server_name, target, python_executable=None, env=None, cwd=None, gemini_scope="project")`
- `serve_main(argv=None)`
- `config_main(argv=None)`

Accepted launcher targets:

- `package.module:server`
- `package.module:build_server`
- `./local_server.py:server`
- `./local_server.py:create_server`

Resolved targets may be:

- `MCPServer`
- a zero-argument factory returning `MCPServer`
- `CompiledStateGraph`
- `StateGraph`
- a tool registry compatible with `MCPServer.from_tool_registry(...)`

#### `MCPServer`

```python
MCPServer(
    *,
    name="agentcore-mcp",
    version="0.1",
    instructions="",
    request_timeout=30.0,
    enable_logging=True,
    resource_subscriptions_enabled=True,
    list_changed_notifications_enabled=True,
    auto_notify_catalog_changes=True,
)
```

Current surface:

- `.add_tool(name, handler, *, description="", input_schema=None, title="", annotations=None)`
- `.tool(name=None, *, description="", input_schema=None, title="", annotations=None)`
- `.list_tools()`
- `.call_tool(name, arguments=None)`
- `.current_session()`
- `.list_client_roots(*, timeout=None)`
- `.sample(messages, *, model_preferences=None, system_prompt=None, include_context=None, temperature=None, max_tokens=None, stop_sequences=None, metadata=None, timeout=None)`
- `.elicit(message, requested_schema, *, timeout=None)`
- `.log(level, data, *, logger=None)`
- `.notify_tools_changed()`
- `.notify_prompts_changed()`
- `.notify_resources_changed()`
- `.notify_resource_updated(uri)`
- `.add_prompt(name, handler, *, description="", arguments=None, title="", argument_completions=None)`
- `.add_prompt_template(name, template, *, description="", arguments=None, title="", argument_completions=None)`
- `.prompt(name=None, *, description="", arguments=None, title="", argument_completions=None)`
- `.list_prompts()`
- `.get_prompt(name, arguments=None)`
- `.add_resource(uri, handler, *, name="", description="", mime_type="", size=None, annotations=None)`
- `.list_resources()`
- `.read_resource(uri)`
- `.add_resource_template(uri_template, handler, *, name="", description="", mime_type="", annotations=None, argument_completions=None)`
- `.resource_template(uri_template=None, *, name="", description="", mime_type="", annotations=None, argument_completions=None)`
- `.list_resource_templates()`
- `.complete(ref, argument_name, value, *, arguments=None)`
- `.serve_stdio(...)`
- `.run_stdio()`
- `MCPServer.from_tool_registry(tool_registry, *, name="agentcore-tools", version="0.1", descriptions=None, input_schemas=None)`
- `MCPServer.from_compiled_graph(compiled_graph, *, name="agentcore-tools", version="0.1", descriptions=None, input_schemas=None)`

The server implements MCP `initialize`, `ping`, `tools/list`, `tools/call`, `prompts/list`, `prompts/get`, `resources/list`, `resources/templates/list`, `resources/read`, `resources/subscribe`, `resources/unsubscribe`, `completion/complete`, `logging/setLevel`, and the optional client-facing request surfaces `roots/list`, `sampling/createMessage`, and `elicitation/create` over newline-delimited JSON-RPC on `stdio`.

If a handler accepts metadata, the active session is injected under both `metadata["session"]` and `metadata["mcp"]`.

#### `MCPServerSession`

Current surface:

- `.client_capabilities`
- `.client_info`
- `.logging_level`
- `.initialized`
- `.roots_change_count`
- `.subscribed_resources`
- `.list_roots(*, timeout=None)`
- `.sample(messages, *, model_preferences=None, system_prompt=None, include_context=None, temperature=None, max_tokens=None, stop_sequences=None, metadata=None, timeout=None)`
- `.elicit(message, requested_schema, *, timeout=None)`
- `.send_log(level, data, *, logger=None)`
- `.notify_tools_changed()`
- `.notify_prompts_changed()`
- `.notify_resources_changed()`
- `.notify_resource_updated(uri)`
- `.is_resource_subscribed(uri)`

`MCPToolServer` remains available as a compatibility alias for the tool-oriented naming used by the earlier, smaller MCP surface.

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
