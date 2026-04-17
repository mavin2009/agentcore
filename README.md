# AgentCore

AgentCore is a native agent-graph runtime written in C++20. It is organized around a compact execution kernel, typed workflow state, explicit state patches, a multi-worker scheduler, append-only traces, resumable checkpoints, tool/model registries, public stream events, subgraph composition, and knowledge-graph state.

The project is split into a small number of subsystems with clear boundaries:

- graph IR for immutable workflow structure and compiled routing data
- state storage for typed fields, blobs, patch logs, and knowledge-graph data
- execution for stepping, patch commit, trace emission, checkpointing, and resume
- runtime for node execution, scheduling, async wait handling, and adapters
- bindings and examples for the Python API and end-to-end reference programs

## Documentation

The root README is the landing page. The task-oriented guides live under [`./docs/README.md`](./docs/README.md):

- [`./docs/quickstarts/python.md`](./docs/quickstarts/python.md): build the Python bindings, define a graph, invoke it, stream events, and use subgraphs
- [`./docs/quickstarts/cpp.md`](./docs/quickstarts/cpp.md): build and embed the C++ runtime, construct graphs, and run the native examples
- [`./docs/concepts/runtime-model.md`](./docs/concepts/runtime-model.md): execution model, state patches, joins, subgraphs, knowledge graph state, checkpoints, and streaming
- [`./docs/reference/api.md`](./docs/reference/api.md): Python surface summary and the key C++ headers and types
- [`./docs/operations/validation.md`](./docs/operations/validation.md): test, smoke, and benchmark entry points

## Why This Shape

The runtime is intentionally built around a small engine instead of a large orchestration layer.

The execution engine is responsible for a narrow set of operations:

- start a run from a `GraphDefinition`
- execute one scheduled node
- apply its `StatePatch`
- emit trace and checkpoint records
- choose and enqueue follow-on work
- resume from a stored checkpoint snapshot

That separation is visible in the public API in [`./agentcore/include/agentcore/execution/engine.h`](./agentcore/include/agentcore/execution/engine.h). The reason for keeping the engine narrow is straightforward: it keeps control flow predictable, reduces hidden mutation, and makes replay/resume mechanics easier to reason about than if tool logic, model logic, or application policy were embedded in the kernel itself.

Several other design decisions follow from the same goal:

- Flat graph IR: nodes and edges are index-based structs with compiled routing tables. This keeps graph lookup and routing simple and cache-friendly and avoids virtual-dispatch-heavy graph metadata in the hot path.
- Explicit state patches: nodes return a `NodeResult` and a `StatePatch` instead of mutating global state in place. That gives the engine a single commit point for trace emission, checkpointing, and replay.
- Typed hot state plus blob references: durable state lives in `WorkflowState::fields`, while larger payloads live in the `BlobStore`. The intent is to keep frequently-read execution state small while still allowing larger artifacts to flow through the graph.
- Scheduler separated from execution: the scheduler owns task queues, worker threads, and async waiter promotion, while the engine owns semantics. This lets concurrency policy evolve without turning the execution loop into a thread-management subsystem.
- Knowledge graph in the state layer: graph memory is treated as first-class runtime state, not as an external sidecar. That keeps graph-aware workflows, subscriptions, and replay under the same state and checkpoint model as ordinary field updates.

## Implemented Architecture

### Graph IR

The graph layer is defined in [`./agentcore/include/agentcore/graph/graph_ir.h`](./agentcore/include/agentcore/graph/graph_ir.h). `GraphDefinition` holds nodes, edges, lookup tables, compiled routes, and compiled knowledge-subscription indexes.

Node kinds currently include:

- `Compute`
- `Control`
- `Tool`
- `Model`
- `Aggregate`
- `Human`
- `Subgraph`

Node policies include flags for:

- fan-out
- stop-after-node
- join-on-incoming-branches
- join-scope creation
- knowledge-graph-reactive execution

Subgraph composition is part of the graph layer through `SubgraphBinding`, and the helper surface for validating and namespacing subgraphs lives in [`./agentcore/include/agentcore/graph/composition/subgraph.h`](./agentcore/include/agentcore/graph/composition/subgraph.h).

### State

The state layer is defined in [`./agentcore/include/agentcore/state/state_store.h`](./agentcore/include/agentcore/state/state_store.h). It provides:

- `WorkflowState` for indexed typed fields
- `BlobStore` for larger payloads
- `PatchLog` for incremental mutation history
- `StringInterner` for stable interned string identifiers
- `KnowledgeGraphStore` for entity/triple storage

`Value` is a small tagged union defined in [`./agentcore/include/agentcore/core/types.h`](./agentcore/include/agentcore/core/types.h), and supports:

- `std::monostate`
- `int64_t`
- `double`
- `bool`
- `BlobRef`
- `InternedStringId`

The knowledge-graph layer is defined in [`./agentcore/include/agentcore/state/knowledge_graph.h`](./agentcore/include/agentcore/state/knowledge_graph.h). It supports entity upserts, triple upserts, indexed lookup, matching by pattern, neighbor traversal, serialization, and copy-on-write shared backing checks.

This split exists because graph execution tends to mix two very different data profiles:

- compact control state such as counters, routing markers, and status flags
- larger artifacts such as prompts, model outputs, tool payloads, and serialized snapshots

Keeping those categories separate allows the engine to move small typed state through the step loop without repeatedly copying larger buffers.

### Execution

The execution layer is centered on `ExecutionEngine` in [`./agentcore/include/agentcore/execution/engine.h`](./agentcore/include/agentcore/execution/engine.h). The public surface includes:

- `start()`
- `step()`
- `run_to_completion()`
- `resume()`
- `register_graph()`
- checkpoint-policy configuration
- checkpoint persistence loading
- stream event reads

Checkpoints and traces are defined in [`./agentcore/include/agentcore/execution/checkpoint.h`](./agentcore/include/agentcore/execution/checkpoint.h). The implementation stores:

- lightweight checkpoint metadata for every recorded checkpoint
- optional full `RunSnapshot` payloads for resumable checkpoints
- append-only trace events
- optional persisted checkpoint records on disk

Proof digests are available in [`./agentcore/include/agentcore/execution/proof.h`](./agentcore/include/agentcore/execution/proof.h). The current proof surface computes digests over snapshots and trace sequences.

Public streaming is defined in [`./agentcore/include/agentcore/execution/streaming/public_stream.h`](./agentcore/include/agentcore/execution/streaming/public_stream.h). Stream events carry:

- run, graph, node, branch, and checkpoint identifiers
- node status and confidence
- patch counts and flags
- namespace frames for subgraph-aware event paths

This shape is deliberate: traces are append-only for observability, checkpoints are resumable when snapshot payloads are present, and public stream events are derived from trace data rather than maintained as a second independent event system.

### Runtime And Scheduling

The runtime layer contains node execution interfaces, async handling, registries, and scheduling. The scheduler surface is defined in [`./agentcore/include/agentcore/runtime/scheduler.h`](./agentcore/include/agentcore/runtime/scheduler.h).

The current scheduler is composed of:

- `WorkQueue` for ordered runnable tasks
- `WorkerPool` for batched parallel execution
- `AsyncCompletionQueue` for tool/model completion promotion

`Scheduler` exposes ready-queue operations, async waiter registration, completion signaling, and parallel batch execution.

The reason this is split from the engine is to preserve a clean distinction between:

- execution semantics: what a node result means
- execution policy: when and where work runs

That distinction matters once the runtime supports fan-out, joins, async external calls, and subgraph execution, because it prevents concurrency concerns from leaking into node logic or state-commit logic.

### Adapters

The runtime uses registries rather than special-casing tools and models inside the engine. The public request/response types live in:

- [`./agentcore/include/agentcore/runtime/tool_api.h`](./agentcore/include/agentcore/runtime/tool_api.h)
- [`./agentcore/include/agentcore/runtime/model_api.h`](./agentcore/include/agentcore/runtime/model_api.h)

Built-in adapters currently include:

- HTTP tool adapter
- SQLite-style tool adapter
- local model adapter
- HTTP LLM adapter

Those implementations live under `./agentcore/adapters/`.

The adapter boundary is intentionally narrow so that model/tool payloads can move as `BlobRef` values through the state system and execution engine without introducing engine-specific logic for individual external systems.

## Repository Layout

All directories below are relative to the repository root (`./`).

The repository is organized by subsystem rather than by executable:

- `./agentcore/include/agentcore/core`: common runtime types
- `./agentcore/include/agentcore/graph`: graph IR and subgraph composition metadata
- `./agentcore/include/agentcore/state`: workflow state, blobs, patch logs, knowledge graph
- `./agentcore/include/agentcore/runtime`: scheduler, node runtime, async APIs, adapter registries
- `./agentcore/include/agentcore/execution`: engine, checkpoints, proofs, streaming
- `./agentcore/src/graph`: graph compilation and subgraph helpers
- `./agentcore/src/state`: state and knowledge-graph implementations
- `./agentcore/src/runtime`: scheduler, registries, async executor
- `./agentcore/src/execution`: engine, checkpointing, proof, streaming, subgraph runtime
- `./agentcore/src/bindings/python`: CPython bridge and module surface
- `./agentcore/adapters`: concrete tool/model adapters
- `./agentcore/benchmarks`: native benchmark entry points
- `./agentcore/examples`: example programs
- `./agentcore/tests`: module and integration tests
- `./docs`: quickstarts, concepts, reference, and validation guides
- `./python/agentcore`: pure-Python package surface layered over the native module
- `./python/tests`: Python smoke and workflow validation
- `./python/benchmarks`: Python benchmark entry points
- `./cmake`: package configuration templates
- `./packaging`: packaging and consumer smoke material

This layout is meant to keep the dependency direction obvious: graph and state are lower-level, runtime and execution build on top of them, and bindings/adapters sit at the boundary.

## Build

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The exported CMake targets are:

- `agentcore::graph`
- `agentcore::state`
- `agentcore::runtime`
- `agentcore::execution`
- `agentcore::adapters`
- `agentcore::agentcore`

The root project name is `agentcore`, and the package configuration is generated through `./cmake/agentcoreConfig.cmake.in`.

## C++ Usage

The basic execution flow is:

1. construct a `GraphDefinition`
2. bind and sort edges
3. start a run with `ExecutionEngine::start()`
4. drive the run with `step()` or `run_to_completion()`
5. inspect state, checkpoints, traces, and stream events

```cpp
#include "agentcore/execution/engine.h"
#include "agentcore/graph/graph_ir.h"

using namespace agentcore;

enum DemoStateKey : StateKey {
    kMessage = 0
};

NodeResult write_message(ExecutionContext& context) {
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kMessage,
        context.blobs.append_string("hello")
    });
    return NodeResult::success(std::move(patch), 1.0F);
}

NodeResult stop_node(ExecutionContext&) {
    return NodeResult::success();
}

int main() {
    GraphDefinition graph;
    graph.id = 1;
    graph.name = "demo";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{1, NodeKind::Compute, "write_message", 0U, 0U, 0U, write_message, {}},
        NodeDefinition{
            2,
            NodeKind::Control,
            "stop",
            node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            stop_node,
            {}
        }
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1});
    graph.sort_edges_by_priority();

    ExecutionEngine engine(2);
    RunId run_id = engine.start(graph, InputEnvelope{1U});
    RunResult result = engine.run_to_completion(run_id);
    (void)run_id;
    (void)result;
}
```

Examples in [`./agentcore/examples/planner_executor_graph.cpp`](./agentcore/examples/planner_executor_graph.cpp), [`./agentcore/examples/retrieval_graph.cpp`](./agentcore/examples/retrieval_graph.cpp), [`./agentcore/examples/knowledge_graph_workflow.cpp`](./agentcore/examples/knowledge_graph_workflow.cpp), and [`./agentcore/examples/runtime_proof_suite.cpp`](./agentcore/examples/runtime_proof_suite.cpp) exercise planner/executor flow, retrieval, knowledge-graph state, and proof/checkpoint behavior.

## Python API

When `AGENTCORE_BUILD_PYTHON_BINDINGS=ON`, the build emits a package under `./build/python/agentcore`.

The current Python surface is implemented in [`./python/agentcore/graph/state.py`](./python/agentcore/graph/state.py) and exposes:

- `StateGraph`
- `CompiledStateGraph`
- `START`
- `END`
- `Command`

The graph builder currently provides:

- `add_node()`
- `add_edge()`
- `add_conditional_edges()`
- `add_fanout()`
- `add_join()`
- `add_subgraph()`
- `set_entry_point()`
- `compile()`

The compiled graph object provides:

- `invoke()`
- `invoke_with_metadata()`
- `stream()`
- `ainvoke()`
- `ainvoke_with_metadata()`
- `astream()`
- `batch()`
- `abatch()`

Example:

```python
from agentcore.graph import END, START, StateGraph


def step(state, config):
    count = int(state.get("count", 0)) + 1
    return {"count": count}


def route(state, config):
    return END if state["count"] >= 3 else "step"


graph = StateGraph(dict, name="counter", worker_count=2)
graph.add_node("step", step)
graph.add_edge(START, "step")
graph.add_conditional_edges("step", route, {END: END, "step": "step"})

compiled = graph.compile()
final_state = compiled.invoke({"count": 0})
metadata = compiled.invoke_with_metadata({"count": 0})
```

The Python layer focuses on a compact builder surface backed by the native runtime rather than mirroring the entire internal C++ API. The reason is to keep the Python entry point predictable while execution, scheduling, checkpointing, streaming, joins, and subgraph behavior stay in native code.

Subgraph notes:

- Python-defined subgraphs are compiled into native `Subgraph` nodes rather than being interpreted in Python.
- Subgraph stream events carry namespace frames so parent and child execution paths can be distinguished in one trace.
- If a graph uses `add_subgraph(...)`, the supplied `config` must be pickle-serializable so the runtime can propagate it into child runs and nested subgraphs.

## Validation And Benchmarks

The repository contains both module tests and end-to-end runtime checks. The default validation path is:

```bash
ctest --test-dir build --output-on-failure
```

Additional runnable entry points include:

- `./build/agentcore_runtime_benchmark`
- `PYTHONPATH=./build/python python3 ./python/tests/state_graph_api_smoke.py`
- `PYTHONPATH=./build/python python3 ./python/tests/agent_workflows_smoke.py`
- `PYTHONPATH=./build/python python3 ./python/benchmarks/state_graph_api_benchmark.py`

The native benchmark currently exercises:

- multi-worker execution
- routing hot-path behavior
- shared-state fork behavior
- reactive knowledge-graph frontier behavior
- queue indexing behavior
- subgraph and resumable-subgraph execution

These programs are useful both as measurement tools and as regression checks for changes in the scheduling, routing, streaming, and resume seams.

## License

This repository includes a root [`./LICENSE`](./LICENSE) and [`./NOTICE`](./NOTICE). The project is licensed under `CPAL-1.0` with filled attribution information naming Michael Avina as the Initial Developer and requiring preserved attribution as specified in Exhibit B.
