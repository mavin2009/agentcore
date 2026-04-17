import asyncio

from agentcore.graph import END, START, StateGraph


async def planner(state, config):
    history = list(state.get("history", []))
    next_count = int(state.get("count", 0)) + 1
    history.append(f"step-{next_count}")
    payload = dict(state.get("payload", {}))
    payload["last"] = history[-1]
    configurable = dict(config.get("configurable", {}))
    await asyncio.sleep(0)
    return {
        "count": next_count,
        "history": history,
        "payload": payload,
        "run_label": configurable.get("run_label", "unset"),
        "tag_count": len(config.get("tags", [])),
    }


async def route(state, config):
    await asyncio.sleep(0)
    target_count = int(dict(config.get("configurable", {})).get("target_count", 2))
    return END if state["count"] >= target_count else "planner"


graph = StateGraph(dict, name="python_state_graph_api_smoke", worker_count=2)
graph.add_node("planner", planner)
graph.add_edge(START, "planner")
graph.add_conditional_edges("planner", route, {END: END, "planner": "planner"})
compiled = graph.compile()

final_state = compiled.invoke(
    {
        "count": 0,
        "history": [],
        "payload": {"seed": True},
        "binary": b"agentcore",
    },
    config={
        "configurable": {"run_label": "sync", "target_count": 2},
        "tags": ["smoke", "sync"],
    },
)
assert final_state["count"] == 2
assert final_state["history"] == ["step-1", "step-2"]
assert final_state["payload"] == {"seed": True, "last": "step-2"}
assert final_state["binary"] == b"agentcore"
assert final_state["run_label"] == "sync"
assert final_state["tag_count"] == 2

details = compiled.invoke_with_metadata(
    {"count": 0, "history": [], "payload": {}},
    config={"configurable": {"run_label": "details", "target_count": 2}, "tags": ["meta"]},
)
assert details["summary"]["status"] == "completed"
assert details["summary"]["steps_executed"] >= 2
assert details["proof"]["combined_digest"] != 0
assert [event["node_name"] for event in details["trace"]] == ["planner", "planner"]

events = list(
    compiled.stream(
        {"count": 0, "history": [], "payload": {}},
        config={"configurable": {"run_label": "stream", "target_count": 2}, "tags": ["stream"]},
    )
)
assert [event["node_name"] for event in events] == ["planner", "planner"]
assert all(event["graph_name"] == "python_state_graph_api_smoke" for event in events)

batch_results = compiled.batch(
    [{"count": 0, "history": [], "payload": {}}, {"count": 0, "history": [], "payload": {}}],
    config=[
        {"configurable": {"run_label": "batch-a", "target_count": 2}, "tags": ["batch"]},
        {"configurable": {"run_label": "batch-b", "target_count": 3}, "tags": ["batch"]},
    ],
)
assert [result["count"] for result in batch_results] == [2, 3]
assert [result["run_label"] for result in batch_results] == ["batch-a", "batch-b"]


async def exercise_async_surface():
    async_state = await compiled.ainvoke(
        {"count": 0, "history": [], "payload": {}},
        config={"configurable": {"run_label": "async", "target_count": 3}, "tags": ["async"]},
    )
    assert async_state["count"] == 3
    assert async_state["run_label"] == "async"
    assert async_state["tag_count"] == 1

    async_events = [
        event
        async for event in compiled.astream(
            {"count": 0, "history": [], "payload": {}},
            config={"configurable": {"run_label": "astream", "target_count": 2}, "tags": ["astream"]},
        )
    ]
    assert [event["node_name"] for event in async_events] == ["planner", "planner"]

    async_batch = await compiled.abatch(
        [{"count": 0, "history": [], "payload": {}}, {"count": 0, "history": [], "payload": {}}],
        config=[
            {"configurable": {"run_label": "abatch-a", "target_count": 2}, "tags": ["abatch"]},
            {"configurable": {"run_label": "abatch-b", "target_count": 4}, "tags": ["abatch"]},
        ],
    )
    assert [result["count"] for result in async_batch] == [2, 4]


asyncio.run(exercise_async_surface())

print("python state graph api smoke passed")
