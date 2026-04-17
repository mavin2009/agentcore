import asyncio
import time

from agentcore.graph import END, START, StateGraph


def counter(state, config):
    return {"count": int(state.get("count", 0)) + 1}


def route(state, config):
    target_count = int(dict(config.get("configurable", {})).get("target_count", 64))
    return END if state["count"] >= target_count else "counter"


graph = StateGraph(dict, name="python_state_graph_api_benchmark", worker_count=2)
graph.add_node("counter", counter)
graph.add_edge(START, "counter")
graph.add_conditional_edges("counter", route, {END: END, "counter": "counter"})
compiled = graph.compile()
config = {"configurable": {"target_count": 64}, "tags": ["benchmark"]}

iterations = 200
start_ns = time.perf_counter_ns()
for _ in range(iterations):
    result = compiled.invoke({"count": 0}, config=config)
invoke_end_ns = time.perf_counter_ns()


async def run_async_benchmark():
    async_iterations = 100
    async_start_ns = time.perf_counter_ns()
    async_result = None
    for _ in range(async_iterations):
        async_result = await compiled.ainvoke({"count": 0}, config=config)
    async_end_ns = time.perf_counter_ns()
    return async_iterations, async_result, async_end_ns - async_start_ns


async_iterations, async_result, async_elapsed_ns = asyncio.run(run_async_benchmark())

assert result["count"] == 64
assert async_result["count"] == 64
invoke_elapsed_ns = invoke_end_ns - start_ns
print(f"python_state_graph_api_invoke_total_ns={invoke_elapsed_ns}")
print(f"python_state_graph_api_invoke_avg_ns={invoke_elapsed_ns // iterations}")
print(f"python_state_graph_api_ainvoke_total_ns={async_elapsed_ns}")
print(f"python_state_graph_api_ainvoke_avg_ns={async_elapsed_ns // async_iterations}")
