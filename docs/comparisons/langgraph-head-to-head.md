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

These numbers were generated on April 18, 2026 from this repository with:

- Python `3.9.18`
- Platform `Linux-5.15.167.4-microsoft-standard-WSL2-x86_64-with-glibc2.31`
- CPU `Intel(R) Xeon(R) W-10885M CPU @ 2.40GHz`
- LangGraph `0.6.11`

## Results

### 1. Same-code builder path

This is the most relevant comparison if you want to benchmark an existing StateGraph-style workflow without immediately rewriting it around AgentCore-native features.

| Metric | LangGraph | AgentCore compat | Relative |
| --- | ---: | ---: | ---: |
| Invoke avg latency | `2.454 ms` | `1.472 ms` | AgentCore compat `1.67x` faster |
| Invoke throughput | `407.5/s` | `679.2/s` | - |
| Stream avg latency | `2.735 ms` | `2.284 ms` | AgentCore compat `1.20x` faster |
| Batch avg cost / item | `4.189 ms` | `3.147 ms` | AgentCore compat `1.33x` faster |
| Batch throughput | `238.7 items/s` | `317.7 items/s` | - |
| Stream events / run | `10` | `10` | matched |

### 2. Native runtime features

This section exercises workloads where AgentCore's native API exposes capabilities that are either not first-class in LangGraph or require more manual composition.

| Workload | LangGraph | AgentCore native | Relative |
| --- | ---: | ---: | ---: |
| Long-running persistent specialist memory: avg latency / round | `55.409 ms` | `45.754 ms` | AgentCore native `1.21x` faster |
| Long-running persistent specialist memory: peak RSS | `111.2 MiB` | `56.1 MiB` | AgentCore native used `49.6%` less memory |
| Pause + resume avg latency | `6.637 ms` | `1.673 ms` | AgentCore native `3.97x` faster |
| Direct invoke avg latency | `1.271 ms` | `955.286 us` | AgentCore native `1.33x` faster |
| Direct vs resumed final state | `True` | `True` | both preserved |
| Persistent session fan-out (24 sessions) avg latency | `116.354 ms` | `127.648 ms` | LangGraph `1.10x` faster on this workload |
| Session-tagged child events on fan-out run | `n/a` | `48` | AgentCore emits child-session identities |

## What These Numbers Mean

The current picture is useful, but it is not one-dimensional.

- On the same-code builder path, AgentCore's LangGraph-compatible surface is already faster on invoke, stream, and batch for this workflow.
- On native persistent-session workloads, AgentCore is substantially stronger on pause/resume cost and memory footprint.
- On the current 24-session pure-Python fan-out benchmark, AgentCore is near parity but still slightly slower.

That last result matters. It is the main optimization gap surfaced by this head-to-head run. The benchmark stays in the repo specifically so that future scheduler and subgraph-session changes can be measured against it instead of argued about abstractly.

## Reproduce

From the repository root:

```bash
cmake -S . -B build -DAGENTCORE_BUILD_PYTHON_BINDINGS=ON
cmake --build build -j
python3 -m pip install langgraph
python3 ./python/benchmarks/langgraph_head_to_head.py
```

To capture structured output instead of markdown:

```bash
python3 ./python/benchmarks/langgraph_head_to_head.py --format json
```

## Notes On Fairness

- The "same-code builder path" uses AgentCore's generated LangGraph-compatible surface rather than the native `agentcore.graph` API.
- The "native runtime features" section uses AgentCore's native API because persistent child sessions, explicit output bindings, proof-aware pause/resume, and session-tagged stream metadata are first-class there.
- The fan-out benchmark on the LangGraph side uses child graphs plus `thread_id`-backed persistence inside branch nodes to emulate persistent specialist sessions.
- Results will vary by Python version, CPU, and workload shape. This page should be read as a reproducible benchmark snapshot, not as a universal claim.
