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

## State Is Patch-Based

Nodes do not mutate global workflow state directly. They return a `NodeResult` containing:

- a status
- a `StatePatch`
- optional confidence and flags
- an optional routing override

The engine applies that patch at a single commit point. That is what makes trace emission, replay, and checkpoint generation line up cleanly with state mutation.

## Hot State And Blob State

The state layer intentionally separates:

- small indexed fields in `WorkflowState`
- larger payloads in `BlobStore`
- knowledge-graph state in `KnowledgeGraphStore`

This keeps the hot execution path focused on compact control state while still allowing prompts, model outputs, tool payloads, and serialized artifacts to move through the runtime.

## Concurrency Model

Concurrency is explicit rather than implicit.

- The scheduler manages runnable tasks, worker threads, and async completion promotion.
- The engine remains responsible for execution semantics.
- Fan-out only happens when the graph definition makes it possible.
- Async behavior is primarily for external work such as tools, models, or waiting nodes.

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
- Python config is propagated into child runs
- nested subgraphs work as long as the propagated config remains pickle-serializable

That behavior is covered by [`../../python/tests/agent_workflows_smoke.py`](../../python/tests/agent_workflows_smoke.py) and implemented through the subgraph runtime and Python binding seam.

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
