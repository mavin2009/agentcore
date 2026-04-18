# AgentCore

AgentCore is a native agent-graph runtime written in C++20. It is organized around a compact execution kernel, typed workflow state, explicit state patches, a multi-worker scheduler, append-only traces, resumable checkpoints, tool/model registries, public stream events, persistent subgraph sessions, subgraph composition, and knowledge-graph state.

The project is split into a small number of subsystems with clear boundaries:

- graph IR for immutable workflow structure and compiled routing data
- state storage for typed fields, blobs, patch logs, and knowledge-graph data
- execution for stepping, patch commit, trace emission, checkpointing, and resume
- runtime for node execution, scheduling, async wait handling, and adapters
- bindings and examples for the Python API and end-to-end reference programs

## Documentation

The root README is the landing page. The task-oriented guides live under [`./docs/README.md`](./docs/README.md):

- [`./docs/quickstarts/python.md`](./docs/quickstarts/python.md): build the Python bindings, define a graph, invoke it, stream events, and use persistent subgraph sessions
- [`./docs/quickstarts/cpp.md`](./docs/quickstarts/cpp.md): build and embed the C++ runtime, construct graphs, and run the native examples
- [`./docs/concepts/runtime-model.md`](./docs/concepts/runtime-model.md): execution model, state patches, joins, persistent subgraph sessions, knowledge graph state, checkpoints, and streaming
- [`./docs/reference/api.md`](./docs/reference/api.md): Python surface summary and the key C++ headers and types
- [`./docs/comparisons/langgraph-head-to-head.md`](./docs/comparisons/langgraph-head-to-head.md): measured head-to-head numbers against upstream LangGraph and reproduction commands
- [`./docs/migration/langgraph-to-agentcore.md`](./docs/migration/langgraph-to-agentcore.md): one-page guide for moving a LangGraph-style `StateGraph` to AgentCore
- [`./docs/operations/validation.md`](./docs/operations/validation.md): test, smoke, persistent-session benchmark, and replay validation entry points

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
- Persistent child sessions are stored as isolated child snapshots plus explicit input/output bindings. This keeps concurrent distinct sessions safe, makes same-session conflicts reject cleanly, and prevents hidden mutable state from leaking across parent runs.

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

Subgraph composition is part of the graph layer through `SubgraphBinding`, and the helper surface for validating and namespacing subgraphs lives in [`./agentcore/include/agentcore/graph/composition/subgraph.h`](./agentcore/include/agentcore/graph/composition/subgraph.h). `SubgraphBinding` also carries persistent-session metadata through `session_mode` and `session_id_source_key`, which is what lets one child graph definition be reused across many isolated child sessions.

### State

The state layer is defined in [`./agentcore/include/agentcore/state/state_store.h`](./agentcore/include/agentcore/state/state_store.h). It provides:

- `WorkflowState` for indexed typed fields
- `BlobStore` for larger payloads
- `PatchLog` for incremental mutation history
- `StringInterner` for stable interned string identifiers
- `KnowledgeGraphStore` for entity/triple storage
- `TaskJournal` for persisted once-only side-effect outcomes

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

The task journal exists for a narrower operational reason: some workflows need to perform synchronous external work inside a node body and still remain restart-safe. Rather than pushing idempotency policy into every node, the runtime exposes recorded-effect helpers on `ExecutionContext` and persists those outcomes in the state layer. That keeps effect commitment under the engine’s checkpoint/proof model instead of making replay behavior an ad hoc application concern.

### Execution

The execution layer is centered on `ExecutionEngine` in [`./agentcore/include/agentcore/execution/engine.h`](./agentcore/include/agentcore/execution/engine.h). The public surface includes:

- `start()`
- `step()`
- `run_to_completion()`
- `resume()`
- `resume_run()`
- `interrupt()`
- `apply_state_patch()`
- `inspect()`
- `register_graph()`
- checkpoint-policy configuration
- checkpoint storage selection and loading
- stream event reads

Checkpoints and traces are defined in [`./agentcore/include/agentcore/execution/checkpoint.h`](./agentcore/include/agentcore/execution/checkpoint.h). The implementation stores:

- lightweight checkpoint metadata for every recorded checkpoint
- optional full `RunSnapshot` payloads for resumable checkpoints
- append-only trace events
- optional persisted checkpoint records through pluggable storage backends

The current storage backends are:

- binary file persistence
- SQLite-backed persistence when the build is configured with SQLite support

Proof digests are available in [`./agentcore/include/agentcore/execution/proof.h`](./agentcore/include/agentcore/execution/proof.h). The current proof surface computes digests over snapshots and trace sequences.

Public streaming is defined in [`./agentcore/include/agentcore/execution/streaming/public_stream.h`](./agentcore/include/agentcore/execution/streaming/public_stream.h). Stream events carry:

- run, graph, node, branch, and checkpoint identifiers
- node status and confidence
- patch counts and flags
- namespace frames for subgraph-aware event paths
- `session_id` and `session_revision` for persistent child sessions

This shape is deliberate: traces are append-only for observability, checkpoints are resumable when snapshot payloads are present, and public stream events are derived from trace data rather than maintained as a second independent event system.

### Persistent Subgraph Sessions

Persistent subgraph sessions are implemented in the dedicated subgraph execution seam under [`./agentcore/include/agentcore/execution/subgraph/session_runtime.h`](./agentcore/include/agentcore/execution/subgraph/session_runtime.h) and [`./agentcore/src/execution/subgraph/session_runtime.cpp`](./agentcore/src/execution/subgraph/session_runtime.cpp).

The runtime uses isolated committed child snapshots rather than shared mutable child state. The reason is practical:

- a persistent session can be restored deterministically from its last committed child snapshot
- current parent inputs can be overlaid onto that child state before execution
- successful child completion can commit one full child snapshot revision
- failure, cancellation, or waiting can leave the last committed child snapshot untouched

This is also how the runtime keeps knowledge-graph behavior explicit. Ephemeral subgraphs can receive a per-invocation knowledge-graph fork when propagation is enabled. Persistent sessions seed their child-local knowledge graph once on creation and then continue from their committed child-local graph on later invocations. Parent and child knowledge graphs do not auto-merge outside explicit bindings.

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
- HTTP JSON tool adapter
- SQLite-style tool adapter
- local model adapter
- HTTP LLM adapter
- OpenAI-compatible chat model adapter
- xAI Grok chat model adapter
- Gemini `generateContent` model adapter

Those implementations live under `./agentcore/adapters/`.

The adapter boundary is intentionally narrow so that model/tool payloads can move as `BlobRef` values through the state system and execution engine without introducing engine-specific logic for individual external systems. That same constraint is why the current provider adapters split the way they do: Grok reuses the shared chat-completions HTTP seam because the request/response shape is compatible, while Gemini is implemented as a direct `generateContent` adapter so the runtime can expose its API-key auth and JSON-schema response path without pretending every provider speaks the same protocol.

The registries now also carry an explicit adapter contract through `AdapterMetadata`, transport/auth enums, capability flags, and stable error categorization helpers. That means adapters are no longer just callable handlers; they can describe:

- provider and implementation identity
- transport kind and auth expectations
- request and response formats
- sync/async, structured I/O, checkpoint-safe, JSON-schema, and SQL capabilities
- stable failure categories derived from tool/model response flags

This is the foundation for the next layer of work: richer provider adapters and higher-level Python ergonomics built on top of a discoverable runtime contract rather than ad hoc handler conventions.

### Python Binding Surface

The Python package under `./python/agentcore` is a compact builder and orchestration layer over the native runtime. The low-level graph surface still centers on `StateGraph`, but the package now also includes optional higher-level builders for common workflow shapes under `./python/agentcore/patterns`. Python callbacks may opt into a third `runtime` argument when they need access to native execution services that should stay coupled to the engine rather than reimplemented in Python.

The first exposed service in that seam is recorded once-only synchronous work through `RuntimeContext.record_once(...)` and `RuntimeContext.record_once_with_metadata(...)`. The motivation is operational rather than stylistic: if a callback performs synchronous work that must remain restart-safe, the outcome should be committed through the same native patch/checkpoint path as the rest of the run state. That keeps replay behavior explicit and verifiable instead of depending on ad hoc Python-side memoization.

## Repository Layout

All directories below are relative to the repository root (`./`).

The repository is organized by subsystem rather than by executable:

- `./agentcore/include/agentcore/core`: common runtime types
- `./agentcore/include/agentcore/graph`: graph IR and subgraph composition metadata
- `./agentcore/include/agentcore/state`: workflow state, blobs, patch logs, knowledge graph
- `./agentcore/include/agentcore/state/journal`: persisted task/outcome journal types
- `./agentcore/include/agentcore/runtime`: scheduler, node runtime, async APIs, adapter registries
- `./agentcore/include/agentcore/execution`: engine, checkpoints, proofs, streaming
- `./agentcore/include/agentcore/execution/subgraph`: subgraph execution and session-lifecycle helpers
- `./agentcore/src/graph`: graph compilation and subgraph helpers
- `./agentcore/src/state`: state and knowledge-graph implementations
- `./agentcore/src/runtime`: scheduler, registries, async executor
- `./agentcore/src/execution`: engine, checkpointing, proof, streaming, and higher-level execution seams
- `./agentcore/src/execution/subgraph`: subgraph runtime and persistent-session orchestration
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

## Python Package

The repository also builds a pip-installable wheel through [`./pyproject.toml`](./pyproject.toml). The published distribution name is `agentcore-graph`, while the Python import package remains `agentcore`. The wheel packages the Python API, the native extension, and the compatibility namespace under `agentcore_langgraph_native`.

The simplest way to use AgentCore from Python is to install it directly from PyPI:

```bash
python3 -m pip install agentcore-graph
```

Build the wheel from the repository root:

```bash
CC=cc CXX=c++ python3 -m pip wheel . -w dist
```

Install the built wheel into an isolated target:

```bash
python3 -m pip install --target /tmp/agentcore-wheel-test dist/agentcore_graph-*.whl
PYTHONPATH=/tmp/agentcore-wheel-test python3 -c "from agentcore.graph import StateGraph; print('ok')"
```

The wheel is intentionally focused on the Python runtime surface. It does not publish the top-level `langgraph` shim package, which avoids colliding with an existing upstream `langgraph` installation while still exposing the compatibility layer through `agentcore_langgraph_native.langgraph_compat`.

The repository now also includes release automation under [`.github/workflows/wheels.yml`](./.github/workflows/wheels.yml) and [`.github/workflows/publish-pypi.yml`](./.github/workflows/publish-pypi.yml). The current wheel matrix is Linux `x86_64`, CPython 3.9-3.12, built against `manylinux_2_28` so the published wheels are broadly installable on modern glibc-based Linux systems while source installs remain available as a fallback.

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
- `RuntimeContext`
- `ToolRegistryView`
- `ModelRegistryView`
- `PipelineGraph`
- `PipelineStep`
- `SpecialistTeam`
- `Specialist`

The graph builder currently provides:

- `add_node()`
- `add_edge()`
- `add_conditional_edges()`
- `add_fanout()`
- `add_join()`
- `add_subgraph()`
- `set_entry_point()`
- `compile()`

For users who want a more declarative Python surface without giving up the native execution path, the optional pattern layer under [`./python/agentcore/patterns`](./python/agentcore/patterns) adds:

- `PipelineGraph` for sequential stage pipelines
- `SpecialistTeam` for dispatch/fan-out/aggregate specialist workflows backed by persistent child-session subgraphs

Those helpers still compile into the same native `StateGraph` runtime underneath. The goal is to remove repetitive wiring for common orchestration shapes without introducing a second execution model beside the native core.

The compiled graph object provides:

- `invoke()`
- `invoke_with_metadata()`
- `invoke_until_pause_with_metadata()`
- `resume_with_metadata()`
- `stream()`
- `ainvoke()`
- `ainvoke_with_metadata()`
- `astream()`
- `batch()`
- `abatch()`
- `.tools`
- `.models`

Python callbacks may currently accept:

- `state`
- `(state, config)`
- `(state, config, runtime)`

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

When a callback accepts a third positional argument, the runtime passes a `RuntimeContext`. The current Python-facing helper surface is intentionally small:

- `runtime.available`
- `runtime.record_once(key, request, producer)`
- `runtime.record_once_with_metadata(key, request, producer)`
- `runtime.invoke_tool(name, request, decode="auto")`
- `runtime.invoke_tool_with_metadata(name, request, decode="auto")`
- `runtime.invoke_model(name, prompt, schema=None, max_tokens=0, decode="auto")`
- `runtime.invoke_model_with_metadata(name, prompt, schema=None, max_tokens=0, decode="auto")`

That seam exists for restart-safe synchronous work. If a Python node needs to compute or fetch a value once and then replay the committed outcome on later visits within the same run, the runtime helper routes that through the native task journal rather than asking application code to invent its own replay policy.

Compiled graphs also expose graph-owned adapter registries directly through `.tools` and `.models`. That lets Python code register built-in adapters such as the SQLite-like tool adapter, the HTTP JSON tool adapter, the local model adapter, the OpenAI-compatible chat model adapter, the xAI Grok chat model adapter, and the Gemini `generateContent` model adapter without dropping into C++. The same registry views now also accept custom Python-backed tool/model handlers through `compiled.tools.register(...)` and `compiled.models.register(...)`, with optional payload decoding and metadata overrides. The important part is where those handlers land: they are still registered into the native graph-owned registries, so direct invocation, `RuntimeContext` invocation, registry inspection, and subgraph inheritance all go through the same runtime-owned adapter path rather than a separate Python-only dispatch layer.

Subgraph notes:

- Python-defined subgraphs are compiled into native `Subgraph` nodes rather than being interpreted in Python.
- Subgraph stream events carry namespace frames so parent and child execution paths can be distinguished in one trace.
- `add_subgraph(..., session_mode="persistent", session_id_from="field_name")` enables persistent child-session reuse keyed by a parent field.
- Stream and metadata events expose `session_id` and `session_revision` for persistent child sessions.
- `Command(wait=True)` together with `invoke_until_pause_with_metadata(...)` and `resume_with_metadata(...)` exposes a minimal Python pause/resume seam over the native checkpoint model.
- If a graph uses `add_subgraph(...)`, the supplied `config` must be pickle-serializable so the runtime can propagate it into child runs and nested subgraphs.

## Validation And Benchmarks

The repository contains both module tests and end-to-end runtime checks. The default validation path is:

```bash
cmake -S . -B build -DAGENTCORE_BUILD_PYTHON_BINDINGS=ON -DAGENTCORE_BUILD_BENCHMARKS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Additional runnable entry points include:

- `./build/agentcore_runtime_benchmark`
- `./build/agentcore_persistent_subgraph_session_benchmark`
- `PYTHONPATH=./build/python python3 ./python/tests/state_graph_api_smoke.py`
- `PYTHONPATH=./build/python python3 ./python/tests/agent_workflows_smoke.py`
- `PYTHONPATH=./build/python python3 ./python/tests/patterns_smoke.py`
- `PYTHONPATH=./build/python python3 ./python/tests/adapters_runtime_smoke.py`
- `PYTHONPATH=./build/python python3 ./python/benchmarks/state_graph_api_benchmark.py`
- `python3 ./python/benchmarks/langgraph_head_to_head.py` after installing upstream `langgraph`

The native benchmark currently exercises:

- multi-worker execution
- routing hot-path behavior
- shared-state fork behavior
- reactive knowledge-graph frontier behavior
- queue indexing behavior
- recorded-effect hit/miss cost through the task journal
- subgraph and resumable-subgraph execution
- async multi-wait subgraph execution
- persistent-session fan-out across distinct child sessions
- direct-versus-resumed persistent-session replay validation

The Python benchmark currently exercises:

- synchronous invoke cost
- async invoke cost
- recorded-effect replay cost through the binding layer
- recorded-effect miss cost through the binding layer
- persistent-session fan-out with namespaced and session-tagged events
- pause/resume specialist flows with child-local memory and once-only effects

The optional comparison benchmark in [`./python/benchmarks/langgraph_head_to_head.py`](./python/benchmarks/langgraph_head_to_head.py) publishes a measured snapshot against upstream LangGraph and is summarized in [`./docs/comparisons/langgraph-head-to-head.md`](./docs/comparisons/langgraph-head-to-head.md).

The persistent-session native benchmark is also the replay-validation gate for this feature: it checks direct-versus-resumed output equality, proof-digest equality, committed-session counts, once-only producer counts, and namespaced/session-tagged event coverage. The Python benchmark mirrors those workloads through the binding layer, asserts final-state and session invariants, and emits the corresponding digests for inspection.

## Acknowledgements

AgentCore is an independent project. It is not affiliated with or endorsed by LangChain Inc.

I am grateful to several projects and ideas that helped clarify the design space for graph-based runtime systems:

- [LangGraph](https://github.com/langchain-ai/langgraph) and its [documentation](https://docs.langchain.com/langgraph) for helping make graph-oriented agent orchestration more concrete and widely accessible.
- [NetworkX](https://networkx.org/) for its clean graph-oriented modeling surface and for helping normalize graph-first APIs for developers.
- [Pregel](https://research.google/pubs/pregel-a-system-for-large-scale-graph-processing/) for the larger body of graph-processing ideas around deterministic superstep-style execution.
- [Apache Beam](https://beam.apache.org/) for durable dataflow concepts that continue to influence how many engineers think about structured execution, replay, and checkpointing.

The project also benefits from the broader systems community working on workflow runtimes, replayable execution, state machines, and graph processing.

## License

This repository includes a root [`./LICENSE`](./LICENSE) and [`./NOTICE`](./NOTICE). 
