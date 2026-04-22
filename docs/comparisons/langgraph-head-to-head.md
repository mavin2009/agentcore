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

These numbers were generated on April 22, 2026 from this repository with:

- Python `3.9.18`
- Platform `Linux-5.15.167.4-microsoft-standard-WSL2-x86_64-with-glibc2.31`
- CPU `Intel(R) Xeon(R) W-10885M CPU @ 2.40GHz`
- LangGraph `0.6.11`

## Results

### 1. Same-code builder path

This is the most relevant comparison if you want to benchmark an existing StateGraph-style workflow without immediately rewriting it around AgentCore-native features.

| Metric | LangGraph | AgentCore compat | Relative |
| --- | ---: | ---: | ---: |
| Invoke avg latency | `3.415 ms` | `1.539 ms` | AgentCore compat `2.22x` faster |
| Invoke throughput | `292.8/s` | `649.9/s` | - |
| Stream avg latency | `3.676 ms` | `1.308 ms` | AgentCore compat `2.81x` faster |
| Stream throughput | `272.0/s` | `764.5/s` | - |
| Batch avg cost / item | `5.426 ms` | `1.804 ms` | AgentCore compat `3.01x` faster |
| Batch throughput | `184.3 items/s` | `554.3 items/s` | - |
| Stream events / run | `10` | `10` | matched |

### 2. Native runtime features

This section exercises workloads where AgentCore's native API exposes capabilities that are either not first-class in LangGraph or require more manual composition.

| Workload | LangGraph | AgentCore native | Relative |
| --- | ---: | ---: | ---: |
| Long-running persistent specialist memory: avg latency / round | `66.389 ms` | `5.064 ms` | AgentCore native `13.11x` faster |
| Long-running persistent specialist memory: peak RSS | `111.2 MiB` | `43.2 MiB` | AgentCore native used `61.2%` less memory |
| Pause + resume avg latency | `6.225 ms` | `896.651 us` | AgentCore native `6.94x` faster |
| Direct invoke avg latency | `1.354 ms` | `541.860 us` | AgentCore native `2.50x` faster |
| Direct vs resumed final state | `True` | `True` | both preserved |
| Persistent session fan-out (24 sessions) avg latency | `149.477 ms` | `28.083 ms` | AgentCore native `5.32x` faster |
| Session-tagged child events on fan-out run | `n/a` | `48` | AgentCore emits child-session identities |

## What These Numbers Mean

The current picture is useful, but it is not one-dimensional.

- On the same-code builder path, AgentCore's LangGraph-compatible surface remains ahead on invoke, stream, and batch for this workflow.
- On native persistent-session workloads, AgentCore is materially ahead on latency, memory footprint, pause/resume cost, and direct invoke cost in this snapshot.
- On the current 24-session pure-Python fan-out benchmark, AgentCore remains substantially ahead on latency while still emitting child-session identities directly in the public stream.
- Both runtimes preserved final-state equivalence across the direct-vs-resumed pause workflow in this benchmark.

That mix is exactly why the benchmark stays in the repo. It lets scheduler, state, and subgraph-session changes be measured against a stable comparison instead of argued about abstractly.

## Reproduce

From the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAGENTCORE_BUILD_PYTHON_BINDINGS=ON -DAGENTCORE_BUILD_BENCHMARKS=ON
cmake --build build -j4 --target _agentcore_native agentcore_python_package
python3 -m pip install "langgraph==0.6.11"
PYTHONPATH=./build/python python3 ./python/benchmarks/langgraph_head_to_head.py
```

To capture structured output instead of markdown:

```bash
PYTHONPATH=./build/python python3 ./python/benchmarks/langgraph_head_to_head.py --format json
```

## Notes On Fairness

- The "same-code builder path" uses AgentCore's generated LangGraph-compatible surface rather than the native `agentcore.graph` API.
- The "native runtime features" section uses AgentCore's native API because persistent child sessions, explicit output bindings, proof-aware pause/resume, and session-tagged stream metadata are first-class there.
- The fan-out benchmark on the LangGraph side uses child graphs plus `thread_id`-backed persistence inside branch nodes to emulate persistent specialist sessions.
- Results will vary by Python version, CPU, and workload shape. This page should be read as a reproducible benchmark snapshot, not as a universal claim.
