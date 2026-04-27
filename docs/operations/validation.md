# Validation And Benchmarks

This page collects the commands that are most useful when you need to prove that a change still preserves runtime behavior.

You do not need to run every command after every edit. Pick the lane that matches the risk of the change, then broaden only when the touched code crosses runtime, Python binding, packaging, or benchmark boundaries.

## Pick A Validation Lane

| Change type | Suggested first pass |
| --- | --- |
| README or docs only | `git diff --check` plus any example command you changed |
| Python graph API or examples | local build plus Python smoke tests |
| Native scheduler, state, checkpoint, subgraph, or replay behavior | `release-perf` build, `ctest`, native runtime tests, and native benchmarks |
| MCP or OpenTelemetry integration | local Python smoke tests plus installed-wheel smoke if packaging changed |
| Published package candidate | wheel smoke, `twine check`, and at least one clean install test |
| Benchmark document update | rerun the benchmark command that produced the number and keep build type/profile visible |

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
PYTHONPATH=./build/python python3 ./python/tests/mcp_runtime_smoke.py
PYTHONPATH=./build/python python3 ./python/tests/mcp_launcher_smoke.py
PYTHONPATH=./build/python python3 ./python/tests/opentelemetry_smoke.py
PYTHONPATH=./build/python python3 ./python/tests/context_state_smoke.py
PYTHONPATH=./build/python python3 ./python/tests/real_world_pipeline_test.py
PYTHONPATH=./build/python python3 ./python/tests/graph_store_pipeline_test.py
```

For a real Neo4j adapter validation pass, run Neo4j in Docker and execute the optional integration test. This is the recommended release-candidate check for graph-store changes because it exercises the actual driver and Bolt protocol:

```bash
python3 -m pip install "agentcore-graph[neo4j]"

name="agentcore-neo4j-test"
password="agentcore-test-password"
cleanup() { docker rm -f "$name" >/dev/null 2>&1 || true; }
trap cleanup EXIT

docker rm -f "$name" >/dev/null 2>&1 || true
docker run -d --rm --name "$name" \
  -p 127.0.0.1::7687 \
  -e NEO4J_AUTH="neo4j/$password" \
  neo4j:5-community
port="$(docker port "$name" 7687/tcp | sed -E 's/.*:([0-9]+)$/\1/')"

AGENTCORE_REQUIRE_NEO4J=1 \
AGENTCORE_NEO4J_URI="bolt://127.0.0.1:$port" \
AGENTCORE_NEO4J_USER=neo4j \
AGENTCORE_NEO4J_PASSWORD="$password" \
PYTHONPATH=./build/python \
python3 ./python/tests/neo4j_graph_store_integration_test.py
```

What they cover:

- graph construction and routing
- deterministic node memoization on stable declared inputs
- deterministic node cache invalidation when declared inputs change
- async Python node callbacks
- metadata and streaming surfaces
- graph-native context assembly, native context-graph ranking for intelligence/knowledge selectors, native knowledge-graph context reads, context digests, provenance, conflict metadata, Python fallback behavior, and stream decoration
- an end-to-end incident-style pipeline with retrieval, native knowledge graph writes, persistent specialist subgraphs, context assembly, model invocation, stream metadata, and explicit parent/child knowledge-graph boundaries
- external graph-store hydration into native knowledge, context use of hydrated triples, explicit sync back to the store, and Neo4j adapter relation-parameterization safety
- optional live Neo4j validation for batch writes, neighborhood traversal, filtered query, runtime hydration, context assembly, and sync-back persistence
- Python runtime helper injection and recorded-effect replay
- Python adapter registration, discovery, direct invocation, and runtime invocation
- MCP stdio client handshake, tool discovery, tool calls, prompt retrieval, resource reads, completions, roots, sampling, elicitation, logging control, subscriptions, notifications, and registry mirroring
- OpenTelemetry run spans, node spans, and run/node metrics from the compiled graph API
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
PYTHONPATH=/tmp/agentcore-wheel-test python3 ./python/tests/mcp_runtime_smoke.py
PYTHONPATH=/tmp/agentcore-wheel-test python3 ./python/tests/mcp_launcher_smoke.py
PYTHONPATH=/tmp/agentcore-wheel-test python3 ./python/tests/opentelemetry_smoke.py
PYTHONPATH=/tmp/agentcore-wheel-test python3 ./python/tests/context_state_smoke.py
PYTHONPATH=/tmp/agentcore-wheel-test python3 ./python/tests/real_world_pipeline_test.py
PYTHONPATH=/tmp/agentcore-wheel-test python3 ./python/tests/graph_store_pipeline_test.py
```

That path proves the installed wheel, not the build tree, still supports:

- Python graph construction and execution
- graph-owned adapter registry configuration and invocation
- built-in provider adapter registration for OpenAI-compatible chat, xAI Grok chat, and Gemini `generateContent`
- custom Python-backed adapter handlers routed through the native registry seam
- MCP stdio interoperability, prompt/resource/completion handling, optional client/server MCP features, and mirrored-tool invocation through the installed wheel
- OpenTelemetry observer integration over the installed wheel
- context assembly and context metadata over the installed wheel
- real-world style retrieval, specialist, context, model, and stream pipeline behavior over the installed wheel
- external graph-store hydration and sync over the installed wheel
- multi-agent and subgraph flows
- higher-level pipeline and specialist-team builders
- persistent-session reuse and resume
- streaming and metadata inspection

## Local Manylinux Wheel Smoke

When you need to validate the Linux wheel path before publishing, check the selected build matrix and then run at least one Linux wheel build locally. This requires Docker.

```bash
python3 -m cibuildwheel --platform linux --print-build-identifiers .
CIBW_BUILD=cp39-manylinux_x86_64 python3 -m cibuildwheel --platform linux --output-dir wheelhouse .
```

Then validate the built wheel metadata and installability:

```bash
python3 -m twine check wheelhouse/*.whl
```

Current manual release wheels target Linux `x86_64`, CPython 3.9-3.12, with a `manylinux_2_28` container baseline so C++20 builds remain compatible with a wide range of modern Linux hosts.

GitHub-hosted wheel workflows are intentionally not tracked in the repository right now. Local workflow files can be kept under `.github/workflows/` for later reuse, but `.gitignore` prevents them from being pushed while hosted action capacity is unavailable.

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
- native context-graph cold/warm ranking over intelligence records plus knowledge triples, including selected claim/evidence/knowledge invariants
- native intelligence-query/index regression counters for task lookup, claim-semantic lookup, ranked supporting-claims retrieval, ranked action-candidate retrieval, ranked task agenda, ranked memory recall, bounded focus-set retrieval, first-hop and multi-hop related-record expansion, route selection, and top-match invariants for focused task/claim/memory ranking
- native persistent-session fan-out and resume determinism benchmarks
- native deterministic-node memoization hit/invalidation benchmark with executor-invocation counters
- native recorded-effect hit/miss cost for the task-journal seam
- Python graph invocation cost
- Python context assembly with native-backed `runtime.context.view()` timing and retained Python graph-ranker comparison timing
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

The context-graph benchmark is the structural regression gate for native context retrieval. It checks:

- the native ranker selects from tasks, claims, evidence, decisions, memories, and knowledge triples without changing the public `ContextView` shape
- motif-aware scoring prefers supported claim/evidence/decision/memory/knowledge structures without making model calls or mutating state
- the expected claim, evidence, and knowledge records appear in the selected top-k
- native cold build/rank and cached warm selection counters are emitted by `agentcore_runtime_benchmark`
- the Python benchmark emits both native-backed context view timing and the retained Python graph-ranker timing

Recent Release validation counters from this repository included:

```text
context_graph_records=1024
context_graph_iterations=1024
context_graph_cold_rank_ns=454200
context_graph_warm_rank_avg_ns=52
python_context_graph_warm_view_avg_ns=567840
python_context_graph_python_rank_avg_ns=9231312
```

Treat those as same-machine regression numbers, not portable guarantees.

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
