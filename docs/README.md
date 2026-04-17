# Documentation

AgentCore uses a small set of focused guides instead of one oversized manual.

## Start Here

- [`./quickstarts/python.md`](./quickstarts/python.md): local build, first graph, metadata, streaming, and notes for fan-out, joins, and subgraphs
- [`./quickstarts/cpp.md`](./quickstarts/cpp.md): native build, `ExecutionEngine`, state patches, and example executables
- [`./concepts/runtime-model.md`](./concepts/runtime-model.md): how runs, state, routing, concurrency, subgraphs, and knowledge-graph state fit together
- [`./reference/api.md`](./reference/api.md): Python API summary and the key C++ entry points
- [`./operations/validation.md`](./operations/validation.md): full validation and benchmark commands

## Choose A Path

- If you want to define workflows from Python, start with [`./quickstarts/python.md`](./quickstarts/python.md).
- If you want to embed the runtime in a native application, start with [`./quickstarts/cpp.md`](./quickstarts/cpp.md).
- If you want to reason about execution semantics before writing code, read [`./concepts/runtime-model.md`](./concepts/runtime-model.md).
- If you want the shortest route to build confidence after a change, use [`./operations/validation.md`](./operations/validation.md).

## Living Reference Points

The guides are intentionally backed by executable material in the repository:

- Python smoke coverage: [`../python/tests/state_graph_api_smoke.py`](../python/tests/state_graph_api_smoke.py) and [`../python/tests/agent_workflows_smoke.py`](../python/tests/agent_workflows_smoke.py)
- Native examples: [`../agentcore/examples/planner_executor_graph.cpp`](../agentcore/examples/planner_executor_graph.cpp), [`../agentcore/examples/knowledge_graph_workflow.cpp`](../agentcore/examples/knowledge_graph_workflow.cpp), and [`../agentcore/examples/runtime_proof_suite.cpp`](../agentcore/examples/runtime_proof_suite.cpp)
- Core public headers: [`../agentcore/include/agentcore/graph/graph_ir.h`](../agentcore/include/agentcore/graph/graph_ir.h), [`../agentcore/include/agentcore/execution/engine.h`](../agentcore/include/agentcore/execution/engine.h), and [`../agentcore/include/agentcore/state/state_store.h`](../agentcore/include/agentcore/state/state_store.h)

That split keeps the documentation navigable while still grounding it in code paths that can be built and exercised from this repository.
