# Documentation

AgentCore uses a small set of focused guides instead of one oversized manual.

## Start Here

- [`./quickstarts/python.md`](./quickstarts/python.md): local build, first graph, metadata, streaming, recorded once-only work, persistent subgraph sessions, and pause/resume
- [`./quickstarts/cpp.md`](./quickstarts/cpp.md): native build, `ExecutionEngine`, state patches, and example executables
- [`./concepts/runtime-model.md`](./concepts/runtime-model.md): how runs, state, routing, concurrency, persistent subgraph sessions, and knowledge-graph state fit together
- [`./reference/api.md`](./reference/api.md): Python API summary and the key C++ entry points
- [`./comparisons/langgraph-head-to-head.md`](./comparisons/langgraph-head-to-head.md): measured comparison against upstream LangGraph with reproduction commands
- [`./migration/langgraph-to-agentcore.md`](./migration/langgraph-to-agentcore.md): shortest-path guide for moving a LangGraph-style `StateGraph` to AgentCore
- [`./operations/validation.md`](./operations/validation.md): full validation, smoke, wheel-build, release, persistent-session benchmark commands, and replay invariants

## Choose A Path

- If you want to define workflows from Python, start with [`./quickstarts/python.md`](./quickstarts/python.md).
- If you are moving an existing LangGraph-style graph, start with [`./migration/langgraph-to-agentcore.md`](./migration/langgraph-to-agentcore.md).
- If you want to embed the runtime in a native application, start with [`./quickstarts/cpp.md`](./quickstarts/cpp.md).
- If you want to reason about execution semantics before writing code, read [`./concepts/runtime-model.md`](./concepts/runtime-model.md).
- If you want the shortest route to build confidence after a change, use [`./operations/validation.md`](./operations/validation.md).

## Living Reference Points

The guides are intentionally backed by executable material in the repository:

- Python smoke coverage: [`../python/tests/state_graph_api_smoke.py`](../python/tests/state_graph_api_smoke.py) and [`../python/tests/agent_workflows_smoke.py`](../python/tests/agent_workflows_smoke.py)
- Native examples: [`../agentcore/examples/planner_executor_graph.cpp`](../agentcore/examples/planner_executor_graph.cpp), [`../agentcore/examples/knowledge_graph_workflow.cpp`](../agentcore/examples/knowledge_graph_workflow.cpp), and [`../agentcore/examples/runtime_proof_suite.cpp`](../agentcore/examples/runtime_proof_suite.cpp)
- Benchmarks: [`../agentcore/benchmarks/runtime_benchmark.cpp`](../agentcore/benchmarks/runtime_benchmark.cpp), [`../agentcore/benchmarks/persistent_subgraph_session_benchmark.cpp`](../agentcore/benchmarks/persistent_subgraph_session_benchmark.cpp), [`../python/benchmarks/state_graph_api_benchmark.py`](../python/benchmarks/state_graph_api_benchmark.py), and [`../python/benchmarks/langgraph_head_to_head.py`](../python/benchmarks/langgraph_head_to_head.py)
- Core public headers: [`../agentcore/include/agentcore/graph/graph_ir.h`](../agentcore/include/agentcore/graph/graph_ir.h), [`../agentcore/include/agentcore/execution/engine.h`](../agentcore/include/agentcore/execution/engine.h), and [`../agentcore/include/agentcore/state/state_store.h`](../agentcore/include/agentcore/state/state_store.h)

That split keeps the documentation navigable while still grounding it in code paths that can be built and exercised from this repository.
