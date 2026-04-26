# Runtime Model

This page explains the execution model the codebase implements today. It is meant to help you reason about behavior before you drop into the headers or tests.

## Mental Model

Think of AgentCore as a deterministic commit engine for agent graphs.

Your application describes graph structure and node behavior. During a run, each node observes state, returns a patch, and lets the runtime commit that patch in a known order. The same commit point drives routing, trace events, checkpoints, replay, subgraph-session state, and public metadata.

That model is useful when a workflow has to answer practical production questions:

- What state changed at this step?
- Why did routing choose this edge?
- Can this paused run resume after process restart?
- Did this child subgraph reuse the right session-local memory?
- Can I compare an uninterrupted run with a resumed run?
- Can I stream useful events without inventing a second execution path?

The runtime is intentionally not a prompt policy layer. Prompts, tools, model calls, retrieval, and planning strategies live at the edges. The engine stays focused on graph execution, state commits, scheduling, durability, and inspection.

## Core Separation

AgentCore is organized around a narrow execution kernel with explicit boundaries:

- graph IR describes immutable workflow structure
- state storage owns typed fields, blobs, patch history, and knowledge-graph data
- runtime owns callbacks, registries, scheduling, and async completions
- execution owns stepping, commit, trace emission, checkpointing, and resume

This separation matters because it keeps the engine responsible for semantics instead of turning it into a large policy surface.

## Runs, Nodes, And Steps

At runtime, a run is an execution of one graph with one evolving state snapshot. Each step:

1. loads the scheduled node
2. builds an `ExecutionContext`
3. executes the node callback
4. validates and applies the `StatePatch`
5. emits trace and checkpoint data
6. resolves the next edge or explicit routing override
7. schedules follow-on work or terminates the run

The public engine surface for this lives in [`../../agentcore/include/agentcore/execution/engine.h`](../../agentcore/include/agentcore/execution/engine.h).

`ExecutionEngineOptions` now makes durability and observability policy explicit through `ExecutionProfile`:

- `Strict`: synchronous checkpoint persistence and unbounded trace retention
- `Balanced`: deterministic commit semantics with background checkpoint persistence and per-run segmented traces
- `Fast`: metadata-first checkpointing and bounded trace retention to reduce hot-path durability cost

The point of exposing this at the engine boundary is not to create three different semantics for state mutation. State patches, routing, replay, persistent sessions, and public stream ordering remain explicit runtime behavior in every mode. What changes is how much durability and observability work stays directly on the step path.

## State Is Patch-Based

Nodes do not mutate global workflow state directly. They return a `NodeResult` containing:

- a status
- a `StatePatch`
- optional confidence and flags
- an optional routing override

The engine applies that patch at a single commit point. That is what makes trace emission, replay, and checkpoint generation line up cleanly with state mutation.

There is one deliberate operational exception at the node boundary: recorded synchronous effects. A node can ask `ExecutionContext` to run a keyed effect once and persist its outcome. The node still returns an ordinary `NodeResult`, but the effect record is committed by the engine together with the step. That keeps external once-only work inside the same checkpoint and proof model as patch application instead of scattering replay logic across node bodies.

## Deterministic Node Memoization

The runtime now includes a first incremental-execution seam for deterministic nodes.

- `WorkflowState` tracks per-field revision counters.
- Nodes may declare deterministic memoization metadata through `read_keys`.
- The runtime caches deterministic compute/control/aggregate node results against the current revisions of those declared keys plus the runtime config payload.
- When one of those keys changes, the branch-local memo entries that depend on it are invalidated.

This is intentionally conservative.

- reactive nodes do not currently participate in deterministic memoization
- knowledge-graph or intelligence writes clear the branch-local memo cache rather than trying to derive a partial graph dependency key
- subgraph/tool/model nodes are excluded from this first memoization seam

The reason for this design is straightforward: the runtime gets an explicit invalidation model without weakening replay or smuggling hidden Python-side caches into execution.

## Hot State And Blob State

The state layer intentionally separates:

- small indexed fields in `WorkflowState`
- larger payloads in `BlobStore`
- structured intelligence records in `IntelligenceStore`
- knowledge-graph state in `KnowledgeGraphStore`
- recorded synchronous outcomes in `TaskJournal`

This keeps the hot execution path focused on compact control state while still allowing prompts, model outputs, tool payloads, and serialized artifacts to move through the runtime.

## Context Assembly

AgentCore now exposes a graph-native context assembly layer on the Python runtime surface.

The core idea is that a node can declare a `ContextSpec` at graph-build time and then call `runtime.context.view()` during execution. That view is assembled from the runtime state already available to the node:

- message history from the configured message state field
- task, claim, evidence, decision, and memory records through the native intelligence query surface
- triples from the native `KnowledgeGraphStore` through `runtime.knowledge`
- selected state fields through `state.<field>` selectors
- graph-shaped state carried under `knowledge_graph`, `knowledge`, or `triples` as a compatibility fallback

The context view is not a separate memory store. It is a deterministic read model over committed state plus staged intelligence and knowledge-graph writes visible to the active callback. The returned `ContextView` includes:

- ordered context items
- citation ids
- provenance records
- approximate budget accounting
- conflict metadata for supported or confirmed claims that disagree on the same subject/relation
- a stable digest
- rendering helpers for text prompts, chat messages, and dict/protocol payloads

When context views are created during `invoke_with_metadata(...)`, pause/resume metadata calls, or `stream(...)`, AgentCore attaches a `details["context"]` summary. Matching trace events also receive `context_views` and `context_digest` fields. The native proof digest remains the runtime state/trace proof; the Python metadata layer adds `proof["context_digest"]` so context selection can be compared across runs without hiding that it is a context-view digest.

The current context assembly layer intentionally keeps ranking explicit. It relies on existing native intelligence ranking operations such as `agenda(...)`, `supporting_claims(...)`, `recall(...)`, `focus(...)`, and `action_candidates(...)` rather than introducing a second heuristic ranking engine in Python.

## Intelligence State

AgentCore now includes a first-class intelligence state model inside the native state subsystem rather than treating agent reasoning artifacts as a convention layered on top of generic fields.

The implemented record types are:

- tasks
- claims
- evidence
- decisions
- memories

These records are stored in `IntelligenceStore`, carried through `StatePatch::intelligence`, summarized by `IntelligenceDeltaSummary`, serialized with checkpoints, and restored on replay/resume with the rest of state.

This design is intentionally separate from the knowledge graph.

- the intelligence model is optimized for execution-oriented records such as active work, asserted claims, supporting evidence, accepted or rejected decisions, and memory entries
- the knowledge graph is optimized for entity/triple storage and reactive graph-aware matching

The two can reference the same domain concepts, but they solve different runtime problems.

### Commit And Read Semantics

Intelligence writes follow the same commit rule as other state:

1. the node stages a write into `StatePatch::intelligence`
2. the engine commits that patch once for the step
3. checkpoints, traces, proof digests, and replay all observe the same committed result

On the Python surface, `runtime.intelligence.snapshot()` overlays any staged writes onto the current intelligence store before returning, so a callback can verify read-your-own-writes within the same node execution without introducing a second mutable state channel.

The runtime now also exposes a small operational layer over that store:

- `runtime.intelligence.summary()` for status and layer tallies
- `runtime.intelligence.count(...)` and `runtime.intelligence.exists(...)` for threshold and presence checks
- `runtime.intelligence.query(...)` for filtered record retrieval, including direct claim-semantic filters by `subject`, `relation`, and `object`
- `runtime.intelligence.supporting_claims(...)` for ranked claim retrieval from supporting evidence, decisions, and memories
- `runtime.intelligence.action_candidates(...)` for ranked task retrieval from task, claim, semantic-claim, source, and memory anchors
- `runtime.intelligence.related(...)` for bounded neighborhood expansion around one task or claim
- `runtime.intelligence.agenda(...)` and `runtime.intelligence.next_task(...)` for ranked task selection
- `runtime.intelligence.recall(...)` for ranked memory retrieval
- `runtime.intelligence.focus(...)` for bounded cross-kind retrieval around the current task/memory/evidence frontier, including claim-semantic anchors
- `runtime.intelligence.route(...)` for ordered route selection against the live intelligence view

That layer is intentionally narrow. It gives workflows a way to act on the intelligence records already in state without turning the engine itself into a hardcoded planner or belief system.

The ranking rules are explicit and deterministic:

- task agendas rank tasks by status class (`in_progress`, `open`, `blocked`, `completed`, `cancelled`), then by descending priority, then by descending confidence, then by ascending stable id
- supporting-claim retrieval ranks claims by aligned support first, then total support, then claim status class, then confidence, then stable id
- action-candidate retrieval ranks tasks by direct task anchor first, then direct support alignment from linked evidence/decisions/memories, then task status class, then total linked support, then descending priority, then descending confidence, then stable id
- memory recall ranks memories by descending importance, then by ascending stable id
- focus retrieval seeds from direct anchors plus the current task agenda, memory recall, and explicit source/owner filters, then expands one bounded related set through task/claim links before ranking each record type deterministically
- inside that focus set, direct anchors stay first, and weighted support from selected decisions, stronger evidence, and higher-importance memories is preferred over sheer weak-link volume
- `related(...)` uses the same indexed link structure and can expand for multiple bounded hops (`hops=1` by default), which lets a workflow cross a shared claim or task neighborhood without materializing a custom traversal in Python

That means a workflow can ask for "the next task" or "the top memories" without relying on Python-side sorts or implicit object insertion order.

### Intelligence-Reactive Nodes

Graphs can also declare intelligence-reactive nodes. In the Python layer, this is exposed through `intelligence_subscriptions=[...]` on `StateGraph.add_node(...)`. In the native graph IR, those subscriptions are compiled into node metadata alongside other routing and reactive definitions.

The runtime matches committed `IntelligenceDeltaSummary` entries against those subscriptions after patch commit. Matching nodes are scheduled through the existing reactive frontier machinery:

- the trigger happens only after the intelligence patch is committed
- duplicate no-op writes do not retrigger the frontier
- pending reruns are checkpointed and restored the same way as other reactive frontiers
- the matching decision uses the committed post-step record shape, not a Python-side cache
- claim subscriptions can match on `subject`, `relation`, and `object` directly, and those semantic filters also work with `kind=all` when the intent is "react only to claim deltas with this graph pattern"

This keeps intelligence-triggered reruns deterministic and replayable rather than turning them into out-of-band callbacks or ad hoc observer logic.

### Why This Lives In Native State

The point of this model is not only convenience. It gives the runtime one place to preserve:

- deterministic commit timing
- checkpoint and replay behavior
- subgraph/session isolation
- stream and metadata inspection
- memoization invalidation when structured reasoning state changes

Because intelligence writes are part of ordinary patch application, they also participate in the same persistent session rules as other child state in subgraphs.

## Concurrency Model

Concurrency is explicit rather than implicit.

- The scheduler manages runnable tasks, worker threads, and async completion promotion.
- The engine remains responsible for execution semantics.
- Fan-out only happens when the graph definition makes it possible.
- Async behavior is primarily for external work such as tools, models, or waiting nodes.

The current scheduler keeps three distinct concerns separate:

- per-run ready queues for immediately runnable work
- a delayed-task heap for future `ready_at_ns` wakeups
- async completion queues for tool/model waits

That separation keeps the common dequeue path cache-friendly while still letting the engine wait on the next due task instead of polling.

There is one additional determinism rule at the async boundary: a grouped async wait is treated as one checkpointed frontier, not as a race between unrelated handles. The scheduler only makes that task runnable once the full registered wait group is complete. This matters most for resumable subgraphs, where replay should follow the same child frontier after restore rather than depending on which external handle completed first.

In the Python layer, `worker_count` controls native worker parallelism inside one compiled graph. Separately, `abatch(...)` allows multiple invocations to be active concurrently at the outer API layer.

## Fan-Out And Joins

Branching and joins are modeled in graph structure and node policy rather than through hidden runtime conventions.

- fan-out nodes create explicit parallel branches
- join nodes wait for incoming branches and apply merge rules
- merge rules decide how shared fields are combined at join time

The current Python merge strategies include:

- `require_equal`
- `require_single_writer`
- `last_writer_wins`
- `first_writer_wins`
- `sum_int64`
- `max_int64`
- `min_int64`
- `logical_or`
- `logical_and`
- `concat_sequence`
- `merge_messages`

`concat_sequence` is the native fast path behind `Annotated[list[T], operator.add]`. Python list values are stored as tagged sequence blobs, and the join barrier concatenates those records in deterministic branch order.

`merge_messages` is the native fast path behind `MessagesState` and `Annotated[list[dict], add_messages]`. It stores message history as a tagged message blob. During a join, messages with matching non-empty `id` values replace the existing record while preserving that record's position; messages without ids append. This gives common agent message history the same deterministic commit behavior as other state without requiring a Python reducer callback at the barrier.

The executable smoke coverage for this lives in [`../../python/tests/agent_workflows_smoke.py`](../../python/tests/agent_workflows_smoke.py).

## Subgraphs And Namespaces

Subgraphs are first-class runtime nodes. In the Python builder they are added through `add_subgraph(...)`, compiled into native `Subgraph` nodes, and executed by the same engine.

Important properties of subgraphs in the current implementation:

- parent-to-child field mappings are explicit through input/output bindings
- child stream events carry namespace frames so parent and child paths can be distinguished
- subgraph events and namespace frames carry `session_id` and `session_revision` when a persistent session is in use
- Python config is propagated into child runs
- nested subgraphs work as long as the propagated config remains pickle-serializable

That behavior is covered by [`../../python/tests/agent_workflows_smoke.py`](../../python/tests/agent_workflows_smoke.py) and implemented through the subgraph runtime and Python binding seam.

### Persistent Session Lifecycle

Subgraphs support two session modes:

- ephemeral: each invocation gets a fresh child snapshot
- persistent: a child snapshot is stored and reused by `(subgraph node, session_id)`

For persistent sessions, the lifecycle is:

1. resolve `session_id` from the parent state field configured by `session_id_from`
2. normalize that value to a canonical string key
3. restore the last committed child snapshot for that session, or create a fresh child snapshot if none exists
4. overlay current input bindings onto the child state before execution
5. run the child graph from its entrypoint
6. on successful child completion, commit the full child snapshot back into the parent-managed session table
7. apply declared output bindings from the committed child snapshot back into parent state

Two rules are intentionally strict:

- concurrent reuse of the same `(subgraph node, session_id)` within one run is rejected rather than merged
- child session snapshots only commit on successful completion; failure, cancellation, and waiting leave the last committed snapshot unchanged

This is why persistent sessions are implemented as isolated child snapshots rather than shared mutable subgraph state. The engine gets deterministic restore/commit behavior, and replay can validate the same committed child revisions after restart.

Checkpoint snapshots persist both pending subgraph work and the committed child-session table, including `session_id` and `session_revision`, so cold restore and in-memory resume follow the same lifecycle.

### Knowledge-Graph Propagation In Subgraphs

Knowledge-graph propagation is explicit:

- ephemeral subgraphs get a per-invocation fork when `propagate_knowledge_graph=True`
- persistent sessions seed their child-local knowledge graph only when the session is first created
- later persistent invocations reuse the committed child-local knowledge graph
- parent and child knowledge graphs do not automatically merge outside explicit state/output bindings

That separation matters for graph-aware workflows because it prevents hidden cross-session mutation. A child session can maintain its own reactive frontier and graph-local memory without silently mutating the parent graph state.

The same separation applies to intelligence state. Child intelligence records are part of the child snapshot, not a shared mutable parent side table. Ephemeral subgraphs get a fresh intelligence store per invocation. Persistent subgraphs reuse the committed child-local intelligence store for that session.

## Knowledge-Graph State

Knowledge-graph storage is part of the runtime state model. Nodes can:

- write entities and triples through `StatePatch::knowledge_graph`
- query graph state through `ExecutionContext::knowledge_graph`
- write and query triples from Python through `runtime.knowledge`
- rely on indexed lookup and matching during execution

This is what allows graph-aware workflows to participate in the same trace, checkpoint, and replay model as ordinary field updates.

The reference example for this path is [`../../agentcore/examples/knowledge_graph_workflow.cpp`](../../agentcore/examples/knowledge_graph_workflow.cpp).

## Traces, Checkpoints, And Proofs

The runtime records three related but distinct artifacts:

- trace events for append-only observability
- checkpoint records for resume and persistence
- proof digests for verifying snapshot and trace sequences

Public stream events are derived from runtime trace data rather than maintained as a second unrelated event pipeline. That keeps the observable surface aligned with the execution record.

Checkpoint persistence is now behind a storage backend seam. The runtime currently supports:

- binary file-backed checkpoint storage
- SQLite-backed checkpoint storage when SQLite support is enabled at build time

In `Balanced` and `Fast`, persistence is decoupled from the hot step path through a background append-oriented writer. The engine still assigns checkpoint IDs and commits state synchronously, but durable writes are flushed behind that explicit seam instead of forcing a synchronous full persistence round-trip for every checkpoint record.

Trace retention follows the same direction. Traces are stored per run rather than in one global append vector, which keeps stream reads scoped to one run and avoids unrelated-run scans during public event reads.

Because `TaskJournal` is serialized as part of the state layer, a restored run can reuse previously committed synchronous outcomes without re-executing the underlying side effect. The runtime tests validate both the success path and the failure path where a resumed node presents a different request for the same recorded-effect key.

Persistent child sessions inherit that same rule. Each committed child snapshot carries its own task journal state, so a resumed persistent session can replay previously committed once-only work while still rejecting mismatched requests for the same recorded-effect key.

On top of that persistence layer, the engine exposes an explicit operational flow for intentional intervention:

- `interrupt(run_id)` pauses a live run and captures a resumable snapshot
- `inspect(run_id)` returns the current `RunSnapshot`
- `apply_state_patch(...)` mutates paused state through the same patch model used by nodes
- `resume_run(run_id)` resumes an already-paused in-memory run
- `resume(checkpoint_id)` restores from persisted checkpoint state

## What The Engine Does Not Do

The execution engine intentionally does not contain:

- application-specific prompt logic
- hardcoded tool behavior
- hardcoded model behavior
- graph-specific policy hacks

Those belong in node callbacks, adapters, graph definitions, or higher-level builders. Keeping the kernel narrow is what makes the system easier to validate and easier to embed.

## Related Pages

- Python quickstart: [`../quickstarts/python.md`](../quickstarts/python.md)
- C++ quickstart: [`../quickstarts/cpp.md`](../quickstarts/cpp.md)
- API map: [`../reference/api.md`](../reference/api.md)
- Validation: [`../operations/validation.md`](../operations/validation.md)
