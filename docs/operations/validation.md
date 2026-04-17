# Validation And Benchmarks

This page collects the commands that are most useful when you need to prove that a change still preserves runtime behavior.

## Recommended Full Validation Pass

From the repository root:

```bash
cmake -S . -B build -DAGENTCORE_BUILD_PYTHON_BINDINGS=ON -DAGENTCORE_BUILD_BENCHMARKS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

That path builds the native runtime, examples, benchmarks, and Python bindings, then runs the registered CTest suite.

## Python Smoke Tests

These are useful when you are changing the Python binding seam or graph-builder behavior:

```bash
PYTHONPATH=./build/python python3 ./python/tests/state_graph_api_smoke.py
PYTHONPATH=./build/python python3 ./python/tests/agent_workflows_smoke.py
```

What they cover:

- graph construction and routing
- async Python node callbacks
- metadata and streaming surfaces
- single-agent and multi-agent flows
- fan-out and join behavior
- subgraph composition and nested config propagation
- concurrent execution through `abatch(...)`

## Native Examples

These are useful as executable documentation and spot checks:

```bash
./build/planner_executor_graph
./build/knowledge_graph_workflow
./build/runtime_proof_suite
```

What they cover:

- model-to-tool flow through blob-backed state
- knowledge-graph writes and queries during execution
- proof and checkpoint behavior

## Benchmarks

The native benchmark executable is:

```bash
./build/agentcore_runtime_benchmark
```

If the Python bindings were built, the Python benchmark entry point is:

```bash
PYTHONPATH=./build/python python3 ./python/benchmarks/state_graph_api_benchmark.py
```

These benchmarks are most useful when comparing revisions on the same machine with the same build configuration. They should be treated as regression instruments and change-detection tools, not as universal performance claims.

## Suggested Workflow For Runtime Changes

When changing scheduler, subgraph, or streaming behavior:

1. Run `ctest --test-dir build --output-on-failure`.
2. Run the two Python smoke scripts.
3. Run `./build/agentcore_runtime_benchmark`.
4. If the change touched Python orchestration, also run the Python benchmark.

That sequence gives fast signal on correctness first and performance second.

## Related Pages

- Documentation index: [`../README.md`](../README.md)
- Runtime model: [`../concepts/runtime-model.md`](../concepts/runtime-model.md)
- API map: [`../reference/api.md`](../reference/api.md)
