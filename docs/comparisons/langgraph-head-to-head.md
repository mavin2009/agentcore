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
| Invoke avg latency | `4.793 ms` | `4.123 ms` | AgentCore compat `1.16x` faster |
| Invoke throughput | `208.6/s` | `242.6/s` | - |
| Stream avg latency | `3.547 ms` | `3.764 ms` | LangGraph `1.06x` faster on this workload |
| Batch avg cost / item | `4.578 ms` | `5.043 ms` | LangGraph `1.10x` faster on this workload |
| Batch throughput | `218.4 items/s` | `198.3 items/s` | - |
| Stream events / run | `10` | `10` | matched |

### 2. Native runtime features

This section exercises workloads where AgentCore's native API exposes capabilities that are either not first-class in LangGraph or require more manual composition.

| Workload | LangGraph | AgentCore native | Relative |
| --- | ---: | ---: | ---: |
| Long-running persistent specialist memory: avg latency / round | `59.800 ms` | `40.571 ms` | AgentCore native `1.47x` faster |
| Long-running persistent specialist memory: peak RSS | `109.6 MiB` | `51.6 MiB` | AgentCore native used `52.9%` less memory |
| Pause + resume avg latency | `7.137 ms` | `1.770 ms` | AgentCore native `4.03x` faster |
| Direct invoke avg latency | `1.135 ms` | `1.324 ms` | LangGraph `1.17x` faster on this workload |
| Direct vs resumed final state | `True` | `True` | both preserved |
| Persistent session fan-out (24 sessions) avg latency | `126.517 ms` | `114.382 ms` | AgentCore native `1.11x` faster |
| Session-tagged child events on fan-out run | `n/a` | `48` | AgentCore emits child-session identities |

## What These Numbers Mean

The current picture is useful, but it is not one-dimensional.

- On the same-code builder path, AgentCore's LangGraph-compatible surface is ahead on invoke latency for this workflow, while stream and batch are still slightly behind.
- On native persistent-session workloads, AgentCore is materially stronger on memory footprint and pause/resume cost.
- On the current 24-session pure-Python fan-out benchmark, AgentCore is now slightly ahead after the latest state and join-path optimizations.
- The simplest direct-invoke native micro-workload is still a small optimization gap.

That mix is exactly why the benchmark stays in the repo. It lets scheduler, state, and subgraph-session changes be measured against a stable comparison instead of argued about abstractly.

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
