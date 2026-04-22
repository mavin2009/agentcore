# AgentCore

AgentCore is a native agent-graph runtime written in C++20 with a compact Python surface.

It is built for stateful graph workflows that do more than simple request-response orchestration: branching control flow, tool and model calls, pause/resume behavior, replayable execution, long-lived subgraphs, and workflows that need graph-shaped memory rather than a single opaque state blob.

From Python, AgentCore exposes a `StateGraph`-style builder and compiled runtime API. From C++, it exposes the runtime directly. In both cases, the underlying execution model is the same: graph structure is compiled into native runtime metadata, node results produce explicit state patches, and the engine records checkpoints, traces, and public stream events from that same execution path.

The project is organized around a small core set of subsystems: graph IR, state storage, execution, scheduling, checkpoint/trace infrastructure, and tool/model adapters. The intent is to keep the middle of the runtime understandable while still supporting features such as multi-worker execution, persistent subgraph sessions, deterministic memoization for supported nodes, and knowledge-graph-backed state.

AgentCore is an independent project. It is not affiliated with Amazon Web Services, AWS AgentCore, or any related AWS-branded product or service.

<p align="center">
  <img src="./assets/arch.png" alt="AgentCore architecture" width="820" />
</p>

## What AgentCore Tries To Do

AgentCore keeps the runtime small in the middle and pushes workflow-specific behavior to graph structure, node logic, and adapters.

In practice, that means the runtime is aimed at workflows where you want to inspect state transitions, understand why routing happened, resume interrupted work, or reuse the same subgraph across multiple child sessions without introducing hidden shared mutable state.

That leads to a few practical goals:

- keep the hot path narrow enough to reason about
- make state mutation explicit through patches instead of hidden global writes
- support pause, resume, replay, and inspection without building a second execution model
- let Python users work with a familiar `StateGraph`-style surface while the execution engine stays native
- keep advanced features such as subgraphs, persistent child sessions, and knowledge-graph state inside the same runtime rather than as sidecars

## Architectural Decisions

These are the core choices behind the project.

- Small execution kernel. The engine focuses on node dispatch, patch commit, routing, checkpointing, and resume. Tool logic, model logic, and application policy stay out of the core loop.
- Explicit state patches. Nodes return a `NodeResult` plus a `StatePatch` instead of mutating shared state directly. That gives the runtime one clear commit point for traces, checkpoints, and replay.
- Typed hot state plus blob references. Small workflow state stays cheap to read and update, while larger payloads live out of line.
- Scheduler separated from semantics. The scheduler owns queues, workers, and async wakeups; the engine owns execution meaning and commit order.
- Knowledge graph as runtime state. Graph memory, subscriptions, and graph-aware execution live under the same checkpoint and replay model as ordinary fields.
- Persistent subgraph sessions as isolated child snapshots. Distinct child sessions can run concurrently, while reuse of the same session remains deterministic and resumable.
- Explicit durability profiles. `Strict`, `Balanced`, and `Fast` let users choose how much checkpoint and trace work stays on the hot path without changing state semantics.

## Current Capabilities

- Native C++ runtime with Python bindings
- `StateGraph`-style Python builder and execution surface
- Multi-worker scheduler with async wait handling
- Checkpoints, replay, proof digests, and public stream events
- Subgraph composition with persistent child sessions
- Knowledge-graph-backed state and reactive execution hooks
- Structured intelligence state for tasks, claims, evidence, decisions, and memories
- Deterministic memoization for supported pure nodes
- Tool and model registries with built-in OpenAI-compatible chat, xAI Grok chat, Gemini `generateContent`, HTTP JSON, SQLite-style, and local model adapters
- Validation-focused benchmarks and smoke coverage in both native and Python paths

## Install

For most Python users, the simplest path is:

```bash
python3 -m pip install agentcore-graph
```

The published package name is `agentcore-graph`, and the import package is `agentcore`.

Current published wheels target Linux `x86_64` for CPython `3.9` through `3.12`. Source builds remain available from this repository.

## First Python Graph

```python
from agentcore.graph import END, START, StateGraph


def step(state, config):
    return {"count": int(state.get("count", 0)) + 1}


def route(state, config):
    return END if state["count"] >= 3 else "step"


graph = StateGraph(dict, name="counter", worker_count=2)
graph.add_node("step", step)
graph.add_edge(START, "step")
graph.add_conditional_edges("step", route, {END: END, "step": "step"})

compiled = graph.compile()
final_state = compiled.invoke({"count": 0})
print(final_state)
```

From there, the same compiled graph can also expose metadata, stream events, batch execution, pause/resume, tool and model registries, and persistent subgraph sessions.

## Intelligence State Model

AgentCore also supports a structured intelligence layer inside the same native state system used for ordinary fields, checkpoints, traces, and replay.

The current model stores five related record types:

- tasks
- claims
- evidence
- decisions
- memories

From Python, node callbacks can stage and query intelligence records through `runtime.intelligence`. Those writes remain part of the node's native commit path rather than being stored in a side cache.

```python
from agentcore.graph import RuntimeContext


def analyze(state, config, runtime: RuntimeContext):
    runtime.intelligence.upsert_task(
        "triage",
        title="Triage incoming request",
        owner="planner",
        status="in_progress",
        priority=5,
        details={"request_id": state["request_id"]},
    )
    runtime.intelligence.upsert_claim(
        "customer-needs-followup",
        subject="request",
        relation="requires",
        object="followup",
        status="proposed",
        confidence=0.72,
        statement={"reason": "missing invoice reference"},
    )
    runtime.intelligence.add_evidence(
        "email-fragment",
        kind="message",
        source="support_inbox",
        claim_key="customer-needs-followup",
        content={"excerpt": state["email_excerpt"]},
        confidence=0.81,
    )
    runtime.intelligence.record_decision(
        "route-to-human",
        task_key="triage",
        claim_key="customer-needs-followup",
        status="selected",
        summary={"queue": "billing-review"},
        confidence=0.88,
    )
    runtime.intelligence.remember(
        "customer-tone",
        layer="working",
        scope="request",
        content={"tone": "frustrated"},
        importance=0.6,
    )

    summary = runtime.intelligence.summary()
    supporting_claims = runtime.intelligence.supporting_claims(task_key="triage", limit=3)
    action_candidates = runtime.intelligence.action_candidates(
        owner="planner",
        subject="request",
        relation="requires",
        object="followup",
        limit=2,
    )
    related = runtime.intelligence.related(task_key="triage", hops=2)
    next_task = runtime.intelligence.next_task(owner="planner")
    recalled = runtime.intelligence.recall(scope="request", limit=3)
    focus = runtime.intelligence.focus(owner="planner", scope="request", limit=3)
    return {
        "task_count": summary["counts"]["tasks"],
        "supporting_claims": supporting_claims["counts"]["claims"],
        "best_action_candidate": action_candidates["tasks"][0]["key"],
        "related_claims": related["counts"]["claims"],
        "next_task_key": None if next_task is None else next_task["key"],
        "recalled_memories": recalled["counts"]["memories"],
        "focus_claims": focus["counts"]["claims"],
    }
```

The grouped Python surface is intentionally small:

- `runtime.intelligence.snapshot()`
- `runtime.intelligence.summary()`
- `runtime.intelligence.count(...)`
- `runtime.intelligence.exists(...)`
- `runtime.intelligence.query(...)`
- `runtime.intelligence.supporting_claims(...)`
- `runtime.intelligence.action_candidates(...)`
- `runtime.intelligence.related(...)`
- `runtime.intelligence.agenda(...)`
- `runtime.intelligence.next_task(...)`
- `runtime.intelligence.recall(...)`
- `runtime.intelligence.focus(...)`
- `runtime.intelligence.route(...)`
- `runtime.intelligence.upsert_task(...)`
- `runtime.intelligence.upsert_claim(...)`
- `runtime.intelligence.add_evidence(...)`
- `runtime.intelligence.record_decision(...)`
- `runtime.intelligence.remember(...)`

`focus(...)` is designed to stay operational rather than heuristic-heavy. It anchors on the current task agenda, recall surface, and explicit query filters, expands one bounded related neighborhood, and then favors direct anchors plus decisive support such as selected decisions and strong evidence over raw link count.

`supporting_claims(...)` is the task- and claim-centric ranked retrieval primitive. It returns claims only and orders them by deterministic support from linked evidence, decisions, and memories, so Python code does not have to rebuild that reduction step by hand.

`action_candidates(...)` is the corresponding task-selection primitive. It accepts task, claim, semantic-claim, source, and memory anchors, expands a bounded linked neighborhood, and returns tasks ranked by direct anchor match plus aligned evidence/decision/memory support. That keeps "what should we do next?" retrieval native and deterministic without forcing Python to score a larger mixed snapshot.

`related(...)` stays intentionally compact as well. By default it returns the first-hop neighborhood around a task or claim, and `hops=` can be used to expand farther across the task/claim linkage graph without introducing a second traversal API.

For claim records, `count(...)`, `query(...)`, and `focus(...)` also support `subject=`, `relation=`, and `object=` filters directly. The same semantic filters are available in `IntelligenceRule` and `IntelligenceSubscription`, so claim-centric routing and reactive execution do not need a separate matcher layer in Python.

For conditional edges, the Python layer also exposes `IntelligenceRule` and `IntelligenceRouter` so routing logic can stay declarative while still executing against the native intelligence store.

For intelligence-triggered execution, nodes can also declare `intelligence_subscriptions=[...]` on `StateGraph.add_node(...)`. Those subscriptions compile into native graph metadata and feed the same checkpointed reactive frontier path used by the runtime, rather than relying on a separate Python watcher loop.

`invoke_with_metadata(...)` includes the committed intelligence snapshot under `details["intelligence"]`, so the same structured state can be inspected after execution without reading raw checkpoints.

## Build From Source

For a standard local build:

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For the optimized validation and benchmark path:

```bash
cmake --preset release-perf
cmake --build --preset release-perf -j
ctest --preset release-perf
./build/release-perf/agentcore_runtime_benchmark
./build/release-perf/agentcore_persistent_subgraph_session_benchmark
```

The native runtime benchmark also emits intelligence-query regression counters, including indexed task-query cost, claim-semantic query cost, ranked supporting-claims cost, ranked action-candidate cost, ranked task-agenda cost, ranked memory-recall cost, bounded cross-kind focus-set cost, related-record expansion cost, and route-selection cost over a populated intelligence store.

The repository also includes `relwithdebinfo-perf`, `asan`, `ubsan`, and `tsan` presets.

## How To Navigate The Docs

The main documentation index is [`./docs/README.md`](./docs/README.md). The most useful entry points are:

- [`./docs/quickstarts/python.md`](./docs/quickstarts/python.md) for building the Python bindings, defining graphs, streaming events, using pause/resume, and working with persistent subgraph sessions
- [`./docs/quickstarts/cpp.md`](./docs/quickstarts/cpp.md) for embedding the native runtime from C++
- [`./docs/concepts/runtime-model.md`](./docs/concepts/runtime-model.md) for execution semantics, state, concurrency, sessions, checkpoints, intelligence records, and knowledge-graph behavior
- [`./docs/reference/api.md`](./docs/reference/api.md) for the Python surface and key C++ entry points
- [`./docs/operations/validation.md`](./docs/operations/validation.md) for build, smoke, release, and replay-validation commands
- [`./docs/migration/langgraph-to-agentcore.md`](./docs/migration/langgraph-to-agentcore.md) for moving an existing LangGraph-style graph to AgentCore

## Performance Notes

Current benchmark snapshots and reproduction commands live in [`./docs/comparisons/langgraph-head-to-head.md`](./docs/comparisons/langgraph-head-to-head.md).

The latest published snapshot in this repository was generated on April 22, 2026. On that machine and workload set, AgentCore's compat surface was about `2.22x` to `3.01x` faster on the same-code builder path, while the native persistent-session workloads were about `2.50x` to `13.11x` faster with lower measured memory use. The comparison page includes the exact commands and environment details.

Those numbers are useful as a validation artifact for this repository and for side-by-side comparison on the same workloads, but they should be read as machine- and workload-specific measurements rather than universal claims.

## Repository Map

The repository is organized by subsystem:

- `./agentcore/include` and `./agentcore/src` contain the native runtime
- `./agentcore/adapters` contains built-in tool and model adapters
- `./agentcore/tests` and `./agentcore/benchmarks` contain native validation and benchmark entry points
- `./python/agentcore` contains the Python package layered over the native module
- `./python/tests` and `./python/benchmarks` contain Python smoke tests and benchmark entry points
- `./docs` contains the guides, concepts, reference material, migration notes, and validation docs

## Acknowledgements

AgentCore is an independent project and is not affiliated with or endorsed by LangChain Inc.

I am grateful to the projects and ideas that helped clarify the design space for graph-oriented runtimes:

- [LangGraph](https://github.com/langchain-ai/langgraph) and its [documentation](https://docs.langchain.com/langgraph) for helping make graph-based agent orchestration concrete and accessible
- [NetworkX](https://networkx.org/) for its graph-first modeling vocabulary
- [Pregel](https://research.google/pubs/pregel-a-system-for-large-scale-graph-processing/) for the broader body of ideas around deterministic graph execution
- [Apache Beam](https://beam.apache.org/) for durable dataflow concepts around replay, checkpoints, and structured execution

## License

This repository is licensed under the MIT License. See [`./LICENSE`](./LICENSE) and [`./NOTICE`](./NOTICE).
