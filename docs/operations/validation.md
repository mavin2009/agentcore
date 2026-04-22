# Validation And Benchmarks

This page collects the commands that are most useful when you need to prove that a change still preserves runtime behavior.

## Recommended Full Validation Pass

From the repository root:

```bash
cmake --preset release-perf
cmake --build --preset release-perf -j
ctest --preset release-perf
./build/release-perf/agentcore_runtime_benchmark
./build/release-perf/agentcore_persistent_subgraph_session_benchmark
cmake -S . -B build -DAGENTCORE_BUILD_PYTHON_BINDINGS=ON -DAGENTCORE_BUILD_BENCHMARKS=ON
cmake --build build -j
PYTHONPATH=./build/python python3 ./python/benchmarks/state_graph_api_benchmark.py
PYTHONPATH=./build/python python3 ./python/benchmarks/langgraph_head_to_head.py
```

That path validates the optimized native runtime first, then runs the Python-facing benchmark surfaces. Published native numbers should come from `Release` builds only, so the `release-perf` preset is the default entry point for any benchmark or release-candidate validation pass.

If you need a different native validation lane, the repository also provides:

```bash
cmake --list-presets
```

The current configure presets are:

- `release-perf`
- `relwithdebinfo-perf`
- `asan`
- `ubsan`
- `tsan`

When you want a structured artifact for documentation updates or CI-side parsing, rerun the comparison benchmark with:

```bash
PYTHONPATH=./build/python python3 ./python/benchmarks/langgraph_head_to_head.py --format json
```

This is also the release-candidate path used for the current published benchmark snapshot in [`../comparisons/langgraph-head-to-head.md`](../comparisons/langgraph-head-to-head.md).

## Python Smoke Tests

These are useful when you are changing the Python binding seam or graph-builder behavior:

```bash
PYTHONPATH=./build/python python3 ./python/tests/state_graph_api_smoke.py
PYTHONPATH=./build/python python3 ./python/tests/agent_workflows_smoke.py
PYTHONPATH=./build/python python3 ./python/tests/patterns_smoke.py
PYTHONPATH=./build/python python3 ./python/tests/adapters_runtime_smoke.py
```

What they cover:

- graph construction and routing
- deterministic node memoization on stable declared inputs
- deterministic node cache invalidation when declared inputs change
- async Python node callbacks
- metadata and streaming surfaces
- Python runtime helper injection and recorded-effect replay
- Python adapter registration, discovery, direct invocation, and runtime invocation
- built-in native provider registration for OpenAI-compatible chat, xAI Grok chat, and Gemini `generateContent`
- Python-backed custom tool/model handlers registered into the native registries
- single-agent and multi-agent flows
- higher-level pipeline and specialist-team builders
- fan-out and join behavior
- subgraph composition and nested config propagation
- persistent subgraph session reuse
- parallel specialists targeting the same child graph with different session IDs
- pause/resume preserving child-local memory and streamed session metadata
- concurrent execution through `abatch(...)`

The native runtime module test executable also includes direct scheduler coverage for:

- delayed-task ordering and `next_task_ready_time_for_run(...)`
- single-handle async waiter promotion
- grouped async wait barriers that only promote after the full wait set is complete

## Wheel Packaging Smoke

When you need to validate the publishable Python artifact rather than the local build tree:

```bash
CC=cc CXX=c++ python3 -m pip wheel . -w dist
rm -rf /tmp/agentcore-wheel-test && mkdir -p /tmp/agentcore-wheel-test
python3 -m pip install --no-deps --target /tmp/agentcore-wheel-test dist/agentcore_graph-*.whl
PYTHONPATH=/tmp/agentcore-wheel-test python3 ./python/tests/state_graph_api_smoke.py
PYTHONPATH=/tmp/agentcore-wheel-test python3 ./python/tests/agent_workflows_smoke.py
PYTHONPATH=/tmp/agentcore-wheel-test python3 ./python/tests/patterns_smoke.py
PYTHONPATH=/tmp/agentcore-wheel-test python3 ./python/tests/adapters_runtime_smoke.py
```

That path proves the installed wheel, not the build tree, still supports:

- Python graph construction and execution
- graph-owned adapter registry configuration and invocation
- built-in provider adapter registration for OpenAI-compatible chat, xAI Grok chat, and Gemini `generateContent`
- custom Python-backed adapter handlers routed through the native registry seam
- multi-agent and subgraph flows
- higher-level pipeline and specialist-team builders
- persistent-session reuse and resume
- streaming and metadata inspection

## Cibuildwheel Linux Release Smoke

When you need to validate the release-wheel path that CI uses, check the selected build matrix and then run at least one Linux wheel build locally. This requires Docker.

```bash
python3 -m cibuildwheel --platform linux --print-build-identifiers .
CIBW_BUILD=cp39-manylinux_x86_64 python3 -m cibuildwheel --platform linux --output-dir wheelhouse .
```

Then validate the built wheel metadata and installability:

```bash
python3 -m twine check wheelhouse/*.whl
```

The release automation in `./.github/workflows/wheels.yml` currently targets Linux `x86_64`, CPython 3.9-3.12, with the `manylinux_2_28` container baseline so C++20 builds remain compatible with a wide range of modern Linux hosts.

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

The native benchmark executables are:

```bash
./build/release-perf/agentcore_runtime_benchmark
./build/release-perf/agentcore_persistent_subgraph_session_benchmark
```

If the Python bindings were built, the Python benchmark entry point is:

```bash
PYTHONPATH=./build/python python3 ./python/benchmarks/state_graph_api_benchmark.py
```

If you want the optional public comparison against upstream LangGraph, install LangGraph into the active Python environment and run:

```bash
python3 -m pip install langgraph
PYTHONPATH=./build/python python3 ./python/benchmarks/langgraph_head_to_head.py
```

For a machine-readable report that can be copied directly into documentation updates, use:

```bash
PYTHONPATH=./build/python python3 ./python/benchmarks/langgraph_head_to_head.py --format json
```

For a narrower stress test of wide fan-out/join behavior from Python, there is also:

```bash
PYTHONPATH=./build/python python3 ./python/benchmarks/massive_fanout_benchmark.py --fanout 50 --iterations 10 --workers 4
```

These benchmarks are most useful when comparing revisions on the same machine with the same build configuration. They should be treated as regression instruments and change-detection tools, not as universal performance claims.

For native numbers, do not compare unspecified build types against `Release`. The performance contract for this runtime is now tracked against explicit `release-perf` builds, because durability and observability behavior are profile-dependent and published numbers should always name the profile and the build type.

The current benchmark surfaces include:

- native scheduler, routing, checkpoint, subgraph, and knowledge-frontier benchmarks
- native intelligence-query/index regression counters for task lookup, claim-semantic lookup, ranked supporting-claims retrieval, ranked action-candidate retrieval, ranked task agenda, ranked memory recall, bounded focus-set retrieval, first-hop and multi-hop related-record expansion, route selection, and top-match invariants for focused task/claim/memory ranking
- native persistent-session fan-out and resume determinism benchmarks
- native deterministic-node memoization hit/invalidation benchmark with executor-invocation counters
- native recorded-effect hit/miss cost for the task-journal seam
- Python graph invocation cost
- Python deterministic-node memoization hit/invalidation benchmark through the graph API binding layer
- Python recorded-effect replay and miss cost through the binding layer
- Python persistent-session fan-out, streamed session metadata, and pause/resume state validation
- optional upstream LangGraph vs AgentCore comparison for same-code builder workloads and native persistent-session workloads

The persistent-session native benchmark is the structural replay gate for this feature. It checks:

- equal final output between uninterrupted and resumed runs
- equal proof digest between uninterrupted and resumed runs
- expected committed session counts
- no duplicate once-only producer calls
- presence of namespaced and session-tagged events

The Python benchmark mirrors the same workloads through the binding layer and asserts:

- expected committed session counts
- no duplicate once-only producer calls inside each run
- equal final state between direct and resumed specialist flows
- presence of namespaced and session-tagged events

It also emits the direct/resumed proof digests so Python-side changes can be inspected alongside the native replay gate. Those digests remain useful for spot checks, but the equality gate lives in the native benchmark because the public Python metadata exposes run-specific digests rather than a cross-run-normalized replay checksum.

The deterministic memoization benchmark is the structural regression gate for this seam. It checks:

- equal final output between the baseline run and the stable-input memoized run
- executor invocations collapsing from the configured visit count to a single real node execution on stable declared inputs
- executor invocations remaining at the visit count when a declared read key changes between visits
- trace-event counts remaining fixed across baseline, stable-input, and invalidating variants so the benchmark measures skipped compute rather than altered control flow

## Suggested Workflow For Runtime Changes

When changing scheduler, subgraph, or streaming behavior:

1. Run `cmake --preset release-perf && cmake --build --preset release-perf -j`.
2. Run `ctest --preset release-perf`.
3. Run `./build/agentcore_runtime_module_tests` and `./build/release-perf/agentcore_runtime_module_tests` when the change affects queueing, async waits, or wakeup behavior.
4. Run the four Python smoke scripts from the local `build` tree if the binding layer changed.
5. Run `./build/release-perf/agentcore_runtime_benchmark`.
6. Run `./build/release-perf/agentcore_persistent_subgraph_session_benchmark`.
7. Run `PYTHONPATH=./build/python python3 ./python/benchmarks/langgraph_head_to_head.py` when the change affects scheduler, state, subgraph, or Python runtime behavior. Use `--format json` when you are refreshing the published comparison doc.
8. If the change touched Python orchestration more broadly, also run `PYTHONPATH=./build/python python3 ./python/benchmarks/state_graph_api_benchmark.py`.

That sequence gives fast signal on correctness first and performance second.

For durable-execution changes specifically, also pay attention to:

- checkpoint backend parity between file and SQLite storage
- pause/edit/resume behavior on live runs
- restart-from-checkpoint equivalence after persisted reload
- once-only recorded-effect behavior across restart and replay
- request-mismatch failure behavior for recorded synchronous effects
- persistent child-session commit counts and revision progression
- same-session conflict rejection under parallel scheduling
- namespaced stream events carrying the expected `session_id` and `session_revision`

## Related Pages

- Documentation index: [`../README.md`](../README.md)
- Runtime model: [`../concepts/runtime-model.md`](../concepts/runtime-model.md)
- API map: [`../reference/api.md`](../reference/api.md)
