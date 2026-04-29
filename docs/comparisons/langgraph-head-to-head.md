# AgentCore vs LangGraph

This page records a representative head-to-head benchmark run between upstream LangGraph and AgentCore.

The goal is not to claim universal performance numbers. The goal is narrower:

- show what the current codebase does on one concrete machine
- publish the exact command used to generate the report
- separate the "same-code builder path" from the "native AgentCore features" path

## Benchmark Scope

Two comparison modes are covered:

1. Same-code builder path
   This compares upstream LangGraph to AgentCore's generated LangGraph-compatible surface, `agentcore_langgraph_native.langgraph_compat`, on the same 10-node research workflow.

2. Native runtime features
   This compares upstream LangGraph to AgentCore's native Python API on workloads that rely on persistent sessions, explicit pause/resume, and child-session lifecycle management.

The benchmark entry point is [`../../python/benchmarks/langgraph_head_to_head.py`](../../python/benchmarks/langgraph_head_to_head.py).

## Environment

These numbers were generated on April 29, 2026 from this repository with:

- Python `3.9.18`
- Platform `Linux-5.15.167.4-microsoft-standard-WSL2-x86_64-with-glibc2.31`
- CPU `Intel(R) Xeon(R) W-10885M CPU @ 2.40GHz`
- LangGraph `0.6.11`
- AgentCore build root `./build/release-perf/python`

## Results

### 1. Same-code builder path

This is the most relevant comparison if you want to benchmark an existing StateGraph-style workflow without immediately rewriting it around AgentCore-native features.

| Metric | LangGraph | AgentCore compat | Relative |
| --- | ---: | ---: | ---: |
| Invoke avg latency | `13.208 ms` | `716.199 us` | AgentCore compat `18.44x` faster |
| Invoke throughput | `75.7/s` | `1,396.3/s` | - |
| Stream avg latency | `9.980 ms` | `351.012 us` | AgentCore compat `28.43x` faster |
| Stream throughput | `100.2/s` | `2,848.9/s` | - |
| Batch avg cost / item | `5.884 ms` | `274.951 us` | AgentCore compat `21.40x` faster |
| Batch throughput | `169.9 items/s` | `3,637.0 items/s` | - |
| Stream events / run | `10` | `10` | matched |

### 2. Native runtime features

This section exercises workloads where AgentCore's native API exposes capabilities that are either not first-class in LangGraph or require more manual composition.

| Workload | LangGraph | AgentCore native | Relative |
| --- | ---: | ---: | ---: |
| Long-running persistent specialist memory: avg latency / round | `74.487 ms` | `3.425 ms` | AgentCore native `21.75x` faster |
| Long-running persistent specialist memory: peak RSS | `105.4 MiB` | `30.2 MiB` | AgentCore native used `71.3%` less memory |
| Pause + resume avg latency | `10.611 ms` | `472.280 us` | AgentCore native `22.47x` faster |
| Direct invoke avg latency | `1.814 ms` | `80.096 us` | AgentCore native `22.65x` faster |
| Direct vs resumed final state | `True` | `True` | both preserved |
| Persistent session fan-out (24 sessions) avg latency | `157.192 ms` | `6.182 ms` | AgentCore native `25.43x` faster |
| Session-tagged child events on fan-out run | `n/a` | `48` | AgentCore emits child-session identities |

## What These Numbers Mean

The current picture is useful, but it is not one-dimensional.

- On the same-code builder path, AgentCore's LangGraph-compatible surface remains ahead on invoke, stream, and batch for this workflow.
- On native persistent-session workloads, AgentCore is materially ahead on latency, memory footprint, pause/resume cost, and direct invoke cost in this snapshot.
- On the current 24-session pure-Python fan-out benchmark, AgentCore remains substantially ahead on latency while still emitting child-session identities directly in the public stream.
- Both runtimes preserved final-state equivalence across the direct-vs-resumed pause workflow in this benchmark.

That mix is exactly why the benchmark stays in the repo. It lets scheduler, state, and subgraph-session changes be measured against a stable comparison instead of argued about abstractly.

AgentCore also has native-only regression surfaces for features that are not a same-code framework comparison: context-graph ranking, knowledge ingestion, persistent session replay, deterministic memoization, recorded effects, and stream cursor cost. Those are documented in the [validation guide](../operations/validation.md) because they measure implemented runtime seams rather than replacement imports.

## Reproduce

From the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAGENTCORE_BUILD_PYTHON_BINDINGS=ON -DAGENTCORE_BUILD_BENCHMARKS=ON
cmake --build build -j4 --target _agentcore_native agentcore_python_package
python3 -m pip install "langgraph==0.6.11"
PYTHONPATH=./build/python python3 ./python/benchmarks/langgraph_head_to_head.py
```

For a release-perf build tree, point the benchmark harness at that Python package root:

```bash
cmake --preset release-perf
cmake --build --preset release-perf -j --target _agentcore_native agentcore_python_package
AGENTCORE_BUILD_PYTHON_ROOT=./build/release-perf/python python3 ./python/benchmarks/langgraph_head_to_head.py
```

To capture structured output instead of markdown:

```bash
AGENTCORE_BUILD_PYTHON_ROOT=./build/release-perf/python python3 ./python/benchmarks/langgraph_head_to_head.py --format json
```

## Notes On Fairness

- The "same-code builder path" uses AgentCore's generated LangGraph-compatible surface rather than the native `agentcore.graph` API.
- `AGENTCORE_BUILD_PYTHON_ROOT` lets the benchmark compare against a specific AgentCore build tree while keeping upstream LangGraph imports isolated.
- The "native runtime features" section uses AgentCore's native API because persistent child sessions, explicit output bindings, proof-aware pause/resume, and session-tagged stream metadata are first-class there.
- The fan-out benchmark on the LangGraph side uses child graphs plus `thread_id`-backed persistence inside branch nodes to emulate persistent specialist sessions.
- Native context-graph ranking is measured in `agentcore_runtime_benchmark` and `state_graph_api_benchmark.py`, not in this head-to-head page. That feature depends on AgentCore's structured intelligence and native knowledge state, so it is better treated as an AgentCore-native regression benchmark than as a same-code compatibility comparison.
- Knowledge-ingestion performance is also tracked as an AgentCore-native regression benchmark through `knowledge_ingest_*` counters. It validates structured graph writes, exact lookup, and final store cardinality rather than comparing a shared public API surface.
- Results will vary by Python version, CPU, and workload shape. This page should be read as a reproducible benchmark snapshot, not as a universal claim.

## Related Reading

- Runtime model: [`../concepts/runtime-model.md`](../concepts/runtime-model.md)
- Design lineage and related work: [`../concepts/design-lineage.md`](../concepts/design-lineage.md)
- Validation and benchmark commands: [`../operations/validation.md`](../operations/validation.md)
