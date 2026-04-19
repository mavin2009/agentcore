import argparse
import json
import os
import resource
import time
from typing import Any
from typing_extensions import TypedDict

# Import LangGraph
try:
    from langgraph.graph import END, START, StateGraph as LangGraphStateGraph
except ImportError:
    LangGraphStateGraph = None

# Import AgentCore
import sys
from pathlib import Path
REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "build" / "python"))
sys.path.insert(0, str(REPO_ROOT / "python"))

from agentcore.graph.state import END as AC_END, START as AC_START, StateGraph as ACStateGraph

class State(TypedDict):
    count: int
    results: dict[int, int]
    summary: int

def make_node(index):
    def node(state, config=None):
        # Use separate fields for results to avoid dict merging overhead
        return {f"result_{index}": int(state.get("count", 0)) + index}
    return node

def build_langgraph(fanout):
    if LangGraphStateGraph is None:
        return None
    builder = LangGraphStateGraph(dict)
    def lg_source(state):
        return {"count": 10}
    builder.add_node("source", lg_source)
    for i in range(fanout):
        builder.add_node(f"node_{i}", make_node(i))
    builder.add_edge(START, "source")
    for i in range(fanout):
        builder.add_edge("source", f"node_{i}")
    
    def reducer(state):
        total = 0
        for i in range(fanout):
            total += state.get(f"result_{i}", 0)
        return {"summary": total}
    
    builder.add_node("sink", reducer)
    for i in range(fanout):
        builder.add_edge(f"node_{i}", "sink")
    builder.add_edge("sink", END)
    return builder.compile()

def build_agentcore(fanout, worker_count):
    builder = ACStateGraph(dict, worker_count=worker_count)
    builder.add_fanout("source", lambda state, config: {"count": 10})
    for i in range(fanout):
        builder.add_node(f"node_{i}", make_node(i))
    builder.add_edge(AC_START, "source")
    
    for i in range(fanout):
        builder.add_edge("source", f"node_{i}")
    
    def reducer(state, config):
        total = 0
        for i in range(fanout):
            total += state.get(f"result_{i}", 0)
        return {"summary": total}
    
    builder.add_join("sink", reducer)
    for i in range(fanout):
        builder.add_edge(f"node_{i}", "sink")
    builder.add_edge("sink", AC_END)
    return builder.compile()

def run_bench(name: str, graph: Any, iterations: int):
    if graph is None:
        print(f"{name}: SKIP (not installed)")
        return None
    
    print(f"Running {name} benchmark...")
    start_ns = time.perf_counter_ns()
    for _ in range(iterations):
        res = graph.invoke({"count": 0, "results": {}})
    elapsed_ns = time.perf_counter_ns() - start_ns
    
    avg_ms = (elapsed_ns / iterations) / 1_000_000
    print(f"{name}: Avg Latency = {avg_ms:.2f}ms")
    return avg_ms

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fanout", type=int, default=50)
    parser.add_argument("--iterations", type=int, default=10)
    parser.add_argument("--workers", type=int, default=4)
    args = parser.parse_args()

    print(f"Benchmark Config: Fan-out={args.fanout}, Iterations={args.iterations}, Workers={args.workers}")
    
    lg_graph = build_langgraph(args.fanout)
    ac_graph = build_agentcore(args.fanout, args.workers)

    lg_latency = run_bench("LangGraph", lg_graph, args.iterations)
    ac_latency = run_bench("AgentCore", ac_graph, args.iterations)

    if lg_latency and ac_latency:
        speedup = lg_latency / ac_latency
        print(f"\nRESULT: AgentCore is {speedup:.2f}x faster than LangGraph for {args.fanout} fan-out")

if __name__ == "__main__":
    main()
