import asyncio

from agentcore.graph import Command, END, START, RuntimeContext, StateGraph


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
print(f"DEBUG: final_state['count'] = {final_state.get('count')}")
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
for event in events:
    print(f"DEBUG: event['node_name']={event.get('node_name')}, event['graph_name']='{event.get('graph_name')}'")
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


def unary_step(state):
    return {"count": int(state.get("count", 0)) + 1}


def unary_route(state):
    return END if int(state.get("count", 0)) >= 2 else "step"


unary_graph = StateGraph(dict, name="python_unary_callback_smoke", worker_count=2)
unary_graph.add_node("step", unary_step)
unary_graph.add_edge(START, "step")
unary_graph.add_conditional_edges("step", unary_route, {END: END, "step": "step"})
unary_compiled = unary_graph.compile()

unary_result = unary_compiled.invoke({"count": 0})
assert unary_result["count"] == 2


def patch_cache_step(state, config):
    count = int(state.get("count", 0)) + 1
    return {
        "count": count,
        "shared_total": int(state.get("shared_total", 0)) + count,
        f"dynamic_{count}": f"value-{count}",
    }


def patch_cache_route(state, config):
    return END if int(state.get("count", 0)) >= 2 else "step"


patch_cache_graph = StateGraph(dict, name="python_patch_cache_smoke", worker_count=2)
patch_cache_graph.add_node("step", patch_cache_step)
patch_cache_graph.add_edge(START, "step")
patch_cache_graph.add_conditional_edges("step", patch_cache_route, {END: END, "step": "step"})
patch_cache_compiled = patch_cache_graph.compile()

patch_cache_result = patch_cache_compiled.invoke({"count": 0, "shared_total": 0})
assert patch_cache_result["count"] == 2
assert patch_cache_result["shared_total"] == 3
assert patch_cache_result["dynamic_1"] == "value-1"
assert patch_cache_result["dynamic_2"] == "value-2"

memoized_call_counter = {"stable": 0, "invalidated": 0}


def stable_memoized_node(state, config):
    memoized_call_counter["stable"] += 1
    return {"memo_output": int(state.get("input_value", 0)) * 10}


def stable_router(state, config):
    round_index = int(state.get("memo_round", 0))
    if round_index == 0:
        return Command(update={"memo_round": 1}, goto="memoized")
    return Command(goto=END)


stable_memo_graph = StateGraph(dict, name="python_deterministic_memo_stable", worker_count=2)
stable_memo_graph.add_node(
    "memoized",
    stable_memoized_node,
    deterministic=True,
    read_keys=("input_value",),
    cache_size=8,
)
stable_memo_graph.add_node("router", stable_router, kind="control")
stable_memo_graph.add_edge(START, "memoized")
stable_memo_graph.add_edge("memoized", "router")
stable_memo_compiled = stable_memo_graph.compile()

stable_memo_result = stable_memo_compiled.invoke({"input_value": 7, "memo_round": 0})
assert stable_memo_result["memo_output"] == 70
assert stable_memo_result["memo_round"] == 1
assert memoized_call_counter["stable"] == 1


def invalidating_memoized_node(state, config):
    memoized_call_counter["invalidated"] += 1
    return {"memo_output": int(state.get("input_value", 0)) * 10}


def invalidating_router(state, config):
    round_index = int(state.get("memo_round", 0))
    if round_index == 0:
        return Command(
            update={
                "memo_round": 1,
                "input_value": int(state.get("input_value", 0)) + 1,
            },
            goto="memoized",
        )
    return Command(goto=END)


invalidating_memo_graph = StateGraph(dict, name="python_deterministic_memo_invalidation", worker_count=2)
invalidating_memo_graph.add_node(
    "memoized",
    invalidating_memoized_node,
    deterministic=True,
    read_keys=("input_value",),
    cache_size=8,
)
invalidating_memo_graph.add_node("router", invalidating_router, kind="control")
invalidating_memo_graph.add_edge(START, "memoized")
invalidating_memo_graph.add_edge("memoized", "router")
invalidating_memo_compiled = invalidating_memo_graph.compile()

invalidating_memo_result = invalidating_memo_compiled.invoke({"input_value": 7, "memo_round": 0})
assert invalidating_memo_result["input_value"] == 8
assert invalidating_memo_result["memo_output"] == 80
assert memoized_call_counter["invalidated"] == 2


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


recorded_effect_calls = {"count": 0}
recorded_effect_replays: list[bool] = []
recorded_effect_runtime_available: list[bool] = []


def produce_memoized_value():
    recorded_effect_calls["count"] += 1
    return {
        "value": "cached-result",
        "producer_call": recorded_effect_calls["count"],
    }


def memoized_step(state, config, runtime: RuntimeContext):
    assert runtime.available is True
    recorded_effect_runtime_available.append(runtime.available)

    stable_request = {
        "memo_key": "expensive-lookup",
        "run_label": dict(config.get("configurable", {})).get("run_label", "unset"),
    }

    details = runtime.record_once_with_metadata(
        "python::memoized::expensive-lookup",
        stable_request,
        produce_memoized_value,
    )
    recorded_effect_replays.append(bool(details["replayed"]))

    visit = int(state.get("visit", 0)) + 1
    return {
        "visit": visit,
        "memoized_value": details["value"]["value"],
        "producer_call": details["value"]["producer_call"],
        "replayed": details["replayed"],
    }


def memoized_route(state, config, runtime: RuntimeContext):
    assert runtime.available is True
    return END if int(state.get("visit", 0)) >= 2 else "memoized"


memo_graph = StateGraph(dict, name="python_record_once_smoke", worker_count=2)
memo_graph.add_node("memoized", memoized_step)
memo_graph.add_edge(START, "memoized")
memo_graph.add_conditional_edges(
    "memoized",
    memoized_route,
    {END: END, "memoized": "memoized"},
)
memo_compiled = memo_graph.compile()

memo_result = memo_compiled.invoke(
    {"visit": 0},
    config={"configurable": {"run_label": "record-once-smoke"}},
)
assert memo_result["visit"] == 2
assert memo_result["memoized_value"] == "cached-result"
assert memo_result["producer_call"] == 1
assert memo_result["replayed"] is True
assert recorded_effect_calls["count"] == 1
assert recorded_effect_replays == [False, True]
assert recorded_effect_runtime_available == [True, True]

retained_runtime_state: dict[str, object] = {}


def retain_callback_views(state, config, runtime: RuntimeContext):
    retained_runtime_state["raw_state"] = state
    retained_runtime_state["snapshot"] = state.copy()
    retained_runtime_state["runtime"] = runtime
    return {"captured": int(state.get("count", 0))}


retained_graph = StateGraph(dict, name="python_retained_callback_views_smoke", worker_count=2)
retained_graph.add_node("capture", retain_callback_views)
retained_graph.add_edge(START, "capture")
retained_graph.add_edge("capture", END)
retained_compiled = retained_graph.compile()

retained_result = retained_compiled.invoke({"count": 7})
assert retained_result["captured"] == 7
assert retained_runtime_state["snapshot"] == {"count": 7}

try:
    retained_runtime_state["raw_state"].get("count")
    raise AssertionError("retained callback state should expire after the callback returns")
except RuntimeError:
    pass

try:
    retained_runtime_state["runtime"].record_once_with_metadata(
        "python::expired-runtime::smoke",
        {"count": 7},
        lambda: {"value": "stale"},
    )
    raise AssertionError("retained runtime context should expire after the callback returns")
except RuntimeError:
    pass

print("python state graph api smoke passed")
