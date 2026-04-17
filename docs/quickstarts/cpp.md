# C++ Quickstart

This guide covers the native embedding path: define a `GraphDefinition`, register tools or models when needed, create an `ExecutionEngine`, and run the graph to completion or one step at a time.

## Build

From the repository root:

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The public headers live under [`../../agentcore/include/agentcore`](../../agentcore/include/agentcore), and the root interface target is `agentcore::agentcore`.

## Minimal Execution Flow

At a high level, native usage follows the same five steps every time:

1. Construct a `GraphDefinition`.
2. Register tools or models if the graph needs them.
3. Start a run with `ExecutionEngine::start(...)`.
4. Drive execution with `step(...)` or `run_to_completion(...)`.
5. Inspect final state, traces, and checkpoints.

## Minimal Example

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

NodeResult finish_node(ExecutionContext&) {
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
            "finish",
            node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            finish_node,
            {}
        }
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1});
    graph.sort_edges_by_priority();

    ExecutionEngine engine(2);
    InputEnvelope input;
    input.initial_field_count = 1;

    RunId run_id = engine.start(graph, input);
    RunResult result = engine.run_to_completion(run_id);
    (void)result;
}
```

The key idea is that nodes return `NodeResult` objects containing a `StatePatch`. Nodes do not mutate shared runtime state directly.

## Tools And Models

Tool and model integrations are registered on the engine rather than embedded into graph execution logic:

- `engine.tools().register_tool(...)`
- `engine.models().register_model(...)`

The built example [`../../agentcore/examples/planner_executor_graph.cpp`](../../agentcore/examples/planner_executor_graph.cpp) shows a planner model node feeding a tool node through blob-backed state.

## Knowledge-Graph State

Knowledge-graph data is part of runtime state, not a sidecar service. The example [`../../agentcore/examples/knowledge_graph_workflow.cpp`](../../agentcore/examples/knowledge_graph_workflow.cpp) demonstrates:

- entity writes
- triple writes
- indexed lookup during later nodes
- summary generation from graph state

That flow uses `StatePatch::knowledge_graph` during writes and `ExecutionContext::knowledge_graph` during reads.

## Checkpoints, Proofs, And Streaming

The execution layer also exposes:

- resumable checkpoints
- trace events
- proof digests over snapshots and trace sequences
- public stream events

The example [`../../agentcore/examples/runtime_proof_suite.cpp`](../../agentcore/examples/runtime_proof_suite.cpp) exercises the proof and checkpoint path directly.

## Running The Built Examples

After building, these are useful first commands:

```bash
./build/planner_executor_graph
./build/knowledge_graph_workflow
./build/runtime_proof_suite
```

## Where To Look Next

- Runtime model: [`../concepts/runtime-model.md`](../concepts/runtime-model.md)
- API map: [`../reference/api.md`](../reference/api.md)
- Validation and benchmarks: [`../operations/validation.md`](../operations/validation.md)
