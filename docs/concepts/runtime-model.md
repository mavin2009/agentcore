# Runtime Model

This page explains the execution model the codebase implements today. It is meant to help you reason about behavior before you drop into the headers or tests.

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

- knowledge-reactive nodes do not currently participate in deterministic memoization
- knowledge-graph writes clear the branch-local memo cache rather than trying to derive a partial graph dependency key
- subgraph/tool/model nodes are excluded from this first memoization seam

The reason for this design is straightforward: the runtime gets an explicit invalidation model without weakening replay or smuggling hidden Python-side caches into execution.

## Hot State And Blob State

The state layer intentionally separates:

- small indexed fields in `WorkflowState`
- larger payloads in `BlobStore`
- knowledge-graph state in `KnowledgeGraphStore`
- recorded synchronous outcomes in `TaskJournal`

This keeps the hot execution path focused on compact control state while still allowing prompts, model outputs, tool payloads, and serialized artifacts to move through the runtime.

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

## Knowledge-Graph State

Knowledge-graph storage is part of the runtime state model. Nodes can:

- write entities and triples through `StatePatch::knowledge_graph`
- query graph state through `ExecutionContext::knowledge_graph`
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
