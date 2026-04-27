import asyncio
import time

from agentcore._context_graph import ContextGraphIndex
from agentcore.context import _collect_selector
from agentcore.graph import Command, ContextSpec, END, START, RuntimeContext, StateGraph


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

PATCH_MARSHALLING_ITERATIONS = 200
PAYLOAD_BENCHMARK_ITERATIONS = 100
CONTEXT_GRAPH_BENCHMARK_ITERATIONS = 40
CONTEXT_GRAPH_RECORDS = 48
MEMO_BENCHMARK_ITERATIONS = 64
MEMO_BENCHMARK_VISITS = 64
MEMO_BENCHMARK_ROUNDS = 768
MEMO_BENCHMARK_MASK = (1 << 64) - 1

record_once_calls = {"miss": 0, "hit": 0}
memo_benchmark_calls = {"baseline": 0, "hit": 0, "invalidated": 0}


def produce_record_once_hit_payload():
    record_once_calls["hit"] += 1
    return {"payload": "stable"}


def produce_record_once_miss_payload(count: int):
    record_once_calls["miss"] += 1
    return {"payload": count}


def record_once_hit_counter(state, config, runtime: RuntimeContext):
    count = int(state.get("count", 0)) + 1
    memoized = runtime.record_once(
        "python-benchmark::record-once::shared",
        {"target_count": dict(config.get("configurable", {})).get("target_count", 64)},
        produce_record_once_hit_payload,
    )
    return {"count": count, "memoized": memoized["payload"]}


def record_once_miss_counter(state, config, runtime: RuntimeContext):
    count = int(state.get("count", 0)) + 1
    memoized = runtime.record_once(
        f"python-benchmark::record-once::miss::{count}",
        {"count": count},
        lambda: produce_record_once_miss_payload(count),
    )
    return {"count": count, "memoized": memoized["payload"]}


record_once_hit_graph = StateGraph(dict, name="python_record_once_hit_benchmark", worker_count=2)
record_once_hit_graph.add_node("counter", record_once_hit_counter)
record_once_hit_graph.add_edge(START, "counter")
record_once_hit_graph.add_conditional_edges("counter", route, {END: END, "counter": "counter"})
record_once_hit_compiled = record_once_hit_graph.compile()

record_once_miss_graph = StateGraph(dict, name="python_record_once_miss_benchmark", worker_count=2)
record_once_miss_graph.add_node("counter", record_once_miss_counter)
record_once_miss_graph.add_edge(START, "counter")
record_once_miss_graph.add_conditional_edges("counter", route, {END: END, "counter": "counter"})
record_once_miss_compiled = record_once_miss_graph.compile()


def patch_counter(state, config):
    count = int(state.get("count", 0)) + 1
    return {
        "count": count,
        "score": count * 3,
        "window_start": count,
        "window_end": count + 7,
        "fanout_width": 4,
        "priority": count % 5,
        "ready": (count % 2) == 0,
        "attempts": count,
        "successes": count - 1,
        "failures": 1,
        "label": f"step-{count}",
        "namespace": "patch-benchmark",
    }


def patch_route(state, config):
    target_count = int(dict(config.get("configurable", {})).get("target_count", 64))
    return END if int(state["count"]) >= target_count else "patch_counter"


patch_graph = StateGraph(dict, name="python_patch_marshalling_benchmark", worker_count=2)
patch_graph.add_node("patch_counter", patch_counter)
patch_graph.add_edge(START, "patch_counter")
patch_graph.add_conditional_edges("patch_counter", patch_route, {END: END, "patch_counter": "patch_counter"})
patch_compiled = patch_graph.compile()


def payload_counter(state, config):
    payload = state["payload"]
    items = payload["items"]
    count = int(state.get("count", 0)) + 1
    return {
        "count": count,
        "payload_sum": int(sum(items)),
        "payload_span": int(items[-1] - items[0]),
    }


def payload_route(state, config):
    materialized = state.copy()
    target_count = int(dict(config.get("configurable", {})).get("target_count", 64))
    if int(materialized["payload_sum"]) != 2016:
        raise AssertionError("payload materialization returned an unexpected checksum")
    if int(materialized["payload_span"]) != 63:
        raise AssertionError("payload materialization returned an unexpected span")
    return END if int(materialized["count"]) >= target_count else "payload_counter"


payload_graph = StateGraph(dict, name="python_payload_materialization_benchmark", worker_count=2)
payload_graph.add_node("payload_counter", payload_counter)
payload_graph.add_edge(START, "payload_counter")
payload_graph.add_conditional_edges(
    "payload_counter",
    payload_route,
    {END: END, "payload_counter": "payload_counter"},
)
payload_compiled = payload_graph.compile()


context_graph_timings = {"cold_ns": 0, "warm_ns": 0, "python_rank_ns": 0, "pipeline_ns": 0}


context_graph_spec = ContextSpec(
    goal_key="question",
    include=[
        "tasks.agenda",
        "claims.all",
        "evidence.relevant",
        "decisions.selected",
        "memories.working",
        "knowledge.neighborhood",
    ],
    budget_items=24,
    budget_tokens=2400,
    require_citations=True,
    subject="Incident",
    owner="ops",
    scope="incident",
    task_key="task:incident:8",
    claim_key="claim:incident:8",
)


def context_graph_seed(state, config, runtime: RuntimeContext):
    for index in range(CONTEXT_GRAPH_RECORDS):
        task_key = f"task:incident:{index}"
        claim_key = f"claim:incident:{index}"
        status = "open" if index % 3 else "completed"
        confidence = 0.92 if index == 8 else 0.45 + ((index % 11) / 20)
        runtime.intelligence.upsert_task(
            task_key,
            title=f"Investigate incident path {index}",
            owner="ops" if index % 2 == 0 else "support",
            status=status,
            priority=10 - (index % 10),
            confidence=confidence,
        )
        runtime.intelligence.upsert_claim(
            claim_key,
            subject="Incident",
            relation="affects" if index % 2 == 0 else "mentions",
            object="checkout API" if index == 8 else f"service-{index % 8}",
            status="supported" if index != 5 else "rejected",
            confidence=confidence,
            statement={"path": index, "kind": "context-graph-benchmark"},
        )
        runtime.intelligence.add_evidence(
            f"evidence:incident:{index}",
            kind="log",
            source="observability",
            task_key=task_key,
            claim_key=claim_key,
            content={"signal": f"trace-{index}", "service": f"service-{index % 8}"},
            confidence=confidence,
        )
        runtime.intelligence.record_decision(
            f"decision:incident:{index}",
            task_key=task_key,
            claim_key=claim_key,
            status="selected" if index == 8 else "pending",
            summary={"route": "rollback" if index == 8 else "investigate"},
            confidence=confidence,
        )
        runtime.intelligence.remember(
            f"memory:incident:{index}",
            layer="working",
            scope="incident",
            task_key=task_key,
            claim_key=claim_key,
            content={"lesson": f"path-{index}", "team": "ops"},
            importance=confidence,
        )
        runtime.knowledge.upsert_triple(
            "Incident",
            "affects",
            "checkout API" if index == 8 else f"service-{index % 8}",
            payload={"path": index},
        )
        runtime.knowledge.upsert_triple(
            "checkout API" if index == 8 else f"service-{index % 8}",
            "owned_by",
            "payments team" if index == 8 else f"team-{index % 5}",
            payload={"path": index},
        )
    return {"seeded_context_records": CONTEXT_GRAPH_RECORDS}


def context_graph_viewer(state, config, runtime: RuntimeContext):
    started = time.perf_counter_ns()
    cold_view = runtime.context.view()
    cold_ns = time.perf_counter_ns() - started
    started = time.perf_counter_ns()
    warm_view = runtime.context.view()
    warm_ns = time.perf_counter_ns() - started
    if cold_view.digest != warm_view.digest:
        raise AssertionError("context graph warm view changed the selected context")
    requested_items = int(context_graph_spec.budget_items or 32)
    expanded_limit = max(int(context_graph_spec.limit_per_source), requested_items, 8) * 3
    ranked_limit = max(requested_items, int(context_graph_spec.limit_per_source) * len(context_graph_spec.include))
    expanded_spec = context_graph_spec.replace(limit_per_source=expanded_limit)
    started = time.perf_counter_ns()
    candidates = []
    for selector in context_graph_spec.include or ():
        candidates.extend(_collect_selector(runtime, state, expanded_spec, str(selector)))
    python_ranked = ContextGraphIndex(
        candidates,
        spec=context_graph_spec.to_dict(),
        goal=state.get("question"),
    ).rank(limit=ranked_limit)
    python_rank_ns = time.perf_counter_ns() - started
    if not python_ranked:
        raise AssertionError("python context graph fallback selected no context")
    keys = [item["key"] for item in cold_view.items]
    context_graph_timings["cold_ns"] += cold_ns
    context_graph_timings["warm_ns"] += warm_ns
    context_graph_timings["python_rank_ns"] += python_rank_ns
    return {
        "context_graph_digest": cold_view.digest,
        "context_graph_items": len(cold_view.items),
        "context_graph_keys": keys,
    }


context_graph = StateGraph(dict, name="python_context_graph_benchmark", worker_count=2)
context_graph.add_node("seed", context_graph_seed)
context_graph.add_node("view", context_graph_viewer, context=context_graph_spec)
context_graph.add_edge(START, "seed")
context_graph.add_edge("seed", "view")
context_graph.add_edge("view", END)
context_graph_compiled = context_graph.compile()


def memo_mix(value: int) -> int:
    mixed = (int(value) ^ 0x9E3779B97F4A7C15) & MEMO_BENCHMARK_MASK
    for round_index in range(MEMO_BENCHMARK_ROUNDS):
        mixed ^= mixed >> 33
        mixed &= MEMO_BENCHMARK_MASK
        mixed = (mixed * 0xFF51AFD7ED558CCD) & MEMO_BENCHMARK_MASK
        mixed ^= mixed >> 33
        mixed &= MEMO_BENCHMARK_MASK
        mixed = (mixed + 0x9E3779B97F4A7C15 + round_index) & MEMO_BENCHMARK_MASK
    return mixed & ((1 << 63) - 1)


def build_memoization_benchmark_graph(*, name: str, counter_bucket: str, deterministic: bool, invalidates_input: bool):
    def memoized_node(state, config):
        memo_benchmark_calls[counter_bucket] += 1
        return {"memo_output": memo_mix(int(state.get("memo_input", 0)))}

    def router(state, config):
        visits = int(state.get("memo_visits", 0)) + 1
        updates = {"memo_visits": visits}
        if visits < MEMO_BENCHMARK_VISITS:
            if invalidates_input:
                updates["memo_input"] = int(state.get("memo_input", 0)) + 1
            return Command(update=updates, goto="memoized")
        return Command(update=updates, goto=END)

    graph = StateGraph(dict, name=name, worker_count=2)
    node_kwargs = {}
    if deterministic:
        node_kwargs = {
            "deterministic": True,
            "read_keys": ("memo_input",),
            "cache_size": 64,
        }
    graph.add_node("memoized", memoized_node, **node_kwargs)
    graph.add_node("router", router, kind="control")
    graph.add_edge(START, "memoized")
    graph.add_edge("memoized", "router")
    return graph.compile()


memo_baseline_compiled = build_memoization_benchmark_graph(
    name="python_deterministic_memo_baseline_benchmark",
    counter_bucket="baseline",
    deterministic=False,
    invalidates_input=False,
)
memo_hit_compiled = build_memoization_benchmark_graph(
    name="python_deterministic_memo_hit_benchmark",
    counter_bucket="hit",
    deterministic=True,
    invalidates_input=False,
)
memo_invalidated_compiled = build_memoization_benchmark_graph(
    name="python_deterministic_memo_invalidation_benchmark",
    counter_bucket="invalidated",
    deterministic=True,
    invalidates_input=True,
)


def make_persistent_memo_producer(counter_store: dict[str, int], bucket: str, session_id: str):
    def produce():
        counter_store[bucket] = int(counter_store.get(bucket, 0)) + 1
        return {"memo": f"memo::{session_id}"}

    return produce


def build_persistent_specialist_child_graph(
    *,
    producer_counter_store: dict[str, int],
    producer_bucket: str,
    wait_on_first_visit: bool = False,
):
    def specialist_step(state, config, runtime: RuntimeContext):
        session_id = str(state.get("session_id", "unknown"))
        query = str(state.get("query", ""))
        resume_attempt = int(state.get("resume_attempt", 0))
        base_updates = {}
        if resume_attempt == 0:
            base_updates["resume_attempt"] = 1
        if wait_on_first_visit and resume_attempt == 0:
            return Command(update=base_updates, wait=True)

        visits = int(state.get("visits", 0)) + 1
        memory = list(state.get("memory", []))
        prior_memory = len(memory)
        memory.append(query)
        memoized = runtime.record_once(
            f"python-benchmark::persistent-session::{session_id}",
            {"session_id": session_id},
            make_persistent_memo_producer(producer_counter_store, producer_bucket, session_id),
        )
        updates = {
            "visits": visits,
            "prior_memory": prior_memory,
            "memory": memory,
            "memo": memoized["memo"],
            "answer": f"{session_id}::{query}::visit-{visits}",
        }
        updates.update(base_updates)
        return updates

    child_graph = StateGraph(
        dict,
        name="python_persistent_specialist_child_benchmark",
        worker_count=2,
    )
    child_graph.add_node("specialist_step", specialist_step)
    child_graph.add_edge(START, "specialist_step")
    child_graph.add_edge("specialist_step", END)
    return child_graph


def build_parallel_specialist_graph(
    *,
    session_count: int,
    producer_counter_store: dict[str, int],
):
    child_graph = build_persistent_specialist_child_graph(
        producer_counter_store=producer_counter_store,
        producer_bucket="parallel",
    )

    def join_branches(state, config):
        answers = [state[f"session_{index}_answer"] for index in range(session_count)]
        total_visits = sum(int(state[f"session_{index}_visits"]) for index in range(session_count))
        return {
            "parallel_answers": answers,
            "parallel_total_visits": total_visits,
            "parallel_complete": True,
        }

    graph = StateGraph(
        dict,
        name="python_parallel_persistent_sessions_benchmark",
        worker_count=max(4, session_count),
    )
    graph.add_fanout("fanout")
    graph.add_join("join", join_branches)
    graph.add_edge(START, "fanout")
    graph.add_edge("join", END)

    for index in range(session_count):
        prepare_name = f"prepare_{index}"
        specialist_name = f"specialist_{index}"
        session_field = f"session_{index}_id"
        query_field = f"session_{index}_query"
        answer_field = f"session_{index}_answer"
        visits_field = f"session_{index}_visits"

        def prepare_branch(state, config, *, branch_index=index, session_key=session_field, query_key=query_field):
            return {
                session_key: f"specialist-{branch_index}",
                query_key: f"query-{branch_index}",
            }

        graph.add_node(prepare_name, prepare_branch)
        graph.add_subgraph(
            specialist_name,
            child_graph,
            namespace=specialist_name,
            inputs={
                session_field: "session_id",
                query_field: "query",
            },
            outputs={
                answer_field: "answer",
                visits_field: "visits",
            },
            session_mode="persistent",
            session_id_from=session_field,
        )
        graph.add_edge("fanout", prepare_name)
        graph.add_edge(prepare_name, specialist_name)
        graph.add_edge(specialist_name, "join")

    return graph.compile()


def build_persistent_specialist_parent(
    *,
    producer_counter_store: dict[str, int],
    producer_bucket: str,
    wait_on_first_visit: bool = False,
):
    child_graph = build_persistent_specialist_child_graph(
        producer_counter_store=producer_counter_store,
        producer_bucket=producer_bucket,
        wait_on_first_visit=wait_on_first_visit,
    )

    def revisit_or_finish(state, config):
        round_index = int(state.get("round", 0))
        if round_index == 0:
            return Command(
                update={
                    "round": 1,
                    "query": "followup-brief",
                },
                goto="specialist_session",
            )
        return Command(update={"complete": True}, goto=END)

    parent_graph = StateGraph(
        dict,
        name="python_persistent_specialist_parent_benchmark",
        worker_count=4,
    )
    parent_graph.add_subgraph(
        "specialist_session",
        child_graph,
        namespace="specialist_session",
        inputs={
            "session_id": "session_id",
            "query": "query",
        },
        outputs={
            "answer": "answer",
            "visits": "visits",
            "prior_memory": "prior_memory",
            "memory": "memory",
            "memo": "memo",
            "resume_attempt": "resume_attempt",
        },
        session_mode="persistent",
        session_id_from="session_id",
    )
    parent_graph.add_node("revisit_or_finish", revisit_or_finish)
    parent_graph.add_edge(START, "specialist_session")
    parent_graph.add_edge("specialist_session", "revisit_or_finish")
    parent_graph.add_edge("revisit_or_finish", END)
    return parent_graph.compile()


iterations = 200
start_ns = time.perf_counter_ns()
for _ in range(iterations):
    result = compiled.invoke({"count": 0}, config=config)
invoke_end_ns = time.perf_counter_ns()

record_once_hit_start_ns = time.perf_counter_ns()
for _ in range(iterations):
    record_once_hit_result = record_once_hit_compiled.invoke({"count": 0}, config=config)
record_once_hit_end_ns = time.perf_counter_ns()

record_once_miss_start_ns = time.perf_counter_ns()
for _ in range(iterations):
    record_once_miss_result = record_once_miss_compiled.invoke({"count": 0}, config=config)
record_once_miss_end_ns = time.perf_counter_ns()

patch_start_ns = time.perf_counter_ns()
for _ in range(PATCH_MARSHALLING_ITERATIONS):
    patch_result = patch_compiled.invoke({"count": 0}, config=config)
patch_end_ns = time.perf_counter_ns()

payload_input = {
    "count": 0,
    "payload": {
        "items": list(range(64)),
        "metadata": {
            "kind": "python-payload-materialization",
            "tags": ["benchmark", "payload"],
        },
    },
}
payload_start_ns = time.perf_counter_ns()
for _ in range(PAYLOAD_BENCHMARK_ITERATIONS):
    payload_result = payload_compiled.invoke(dict(payload_input), config=config)
payload_end_ns = time.perf_counter_ns()

context_graph_start_ns = time.perf_counter_ns()
context_graph_result = None
for _ in range(CONTEXT_GRAPH_BENCHMARK_ITERATIONS):
    context_graph_result = context_graph_compiled.invoke(
        {"question": "Which incident path affects checkout API and who owns it?"},
        config={"tags": ["benchmark", "context-graph"]},
    )
context_graph_end_ns = time.perf_counter_ns()

memo_benchmark_input = {"memo_input": 7, "memo_visits": 0}
memo_baseline_start_ns = time.perf_counter_ns()
for _ in range(MEMO_BENCHMARK_ITERATIONS):
    memo_baseline_result = memo_baseline_compiled.invoke(dict(memo_benchmark_input))
memo_baseline_end_ns = time.perf_counter_ns()

memo_hit_start_ns = time.perf_counter_ns()
for _ in range(MEMO_BENCHMARK_ITERATIONS):
    memo_hit_result = memo_hit_compiled.invoke(dict(memo_benchmark_input))
memo_hit_end_ns = time.perf_counter_ns()

memo_invalidated_start_ns = time.perf_counter_ns()
for _ in range(MEMO_BENCHMARK_ITERATIONS):
    memo_invalidated_result = memo_invalidated_compiled.invoke(dict(memo_benchmark_input))
memo_invalidated_end_ns = time.perf_counter_ns()


async def run_async_benchmark():
    async_iterations = 100
    async_start_ns = time.perf_counter_ns()
    async_result = None
    for _ in range(async_iterations):
        async_result = await compiled.ainvoke({"count": 0}, config=config)
    async_end_ns = time.perf_counter_ns()
    return async_iterations, async_result, async_end_ns - async_start_ns


async_iterations, async_result, async_elapsed_ns = asyncio.run(run_async_benchmark())

persistent_session_calls = {"parallel": 0, "direct": 0, "resumed": 0}
parallel_session_count = 8
parallel_iterations = 40
parallel_compiled = build_parallel_specialist_graph(
    session_count=parallel_session_count,
    producer_counter_store=persistent_session_calls,
)
parallel_start_ns = time.perf_counter_ns()
parallel_details = None
for _ in range(parallel_iterations):
    parallel_details = parallel_compiled.invoke_with_metadata(
        {},
        config={"tags": ["benchmark", "persistent-session", "parallel"]},
    )
parallel_end_ns = time.perf_counter_ns()

resume_iterations = 30
direct_resume_compiled = build_persistent_specialist_parent(
    producer_counter_store=persistent_session_calls,
    producer_bucket="direct",
    wait_on_first_visit=False,
)
paused_resume_compiled = build_persistent_specialist_parent(
    producer_counter_store=persistent_session_calls,
    producer_bucket="resumed",
    wait_on_first_visit=True,
)
resume_input = {
    "session_id": "resume-specialist",
    "query": "initial-brief",
    "round": 0,
}

resume_direct_start_ns = time.perf_counter_ns()
direct_resume_details = None
for _ in range(resume_iterations):
    direct_resume_details = direct_resume_compiled.invoke_with_metadata(
        dict(resume_input),
        config={"tags": ["benchmark", "persistent-session", "direct"]},
    )
resume_direct_end_ns = time.perf_counter_ns()

resume_paused_start_ns = time.perf_counter_ns()
paused_resume_details = None
paused_metadata = None
for _ in range(resume_iterations):
    paused_metadata = paused_resume_compiled.invoke_until_pause_with_metadata(
        dict(resume_input),
        config={"tags": ["benchmark", "persistent-session", "resume"]},
    )
    paused_resume_details = paused_resume_compiled.resume_with_metadata(
        paused_metadata["summary"]["checkpoint_id"]
    )
resume_paused_end_ns = time.perf_counter_ns()

assert result["count"] == 64
assert async_result["count"] == 64
assert record_once_hit_result["count"] == 64
assert record_once_hit_result["memoized"] == "stable"
assert record_once_miss_result["count"] == 64
assert record_once_miss_result["memoized"] == 64
assert record_once_calls["hit"] == iterations
assert record_once_calls["miss"] == iterations * 64
assert patch_result["count"] == 64
assert patch_result["score"] == 192
assert patch_result["window_start"] == 64
assert patch_result["window_end"] == 71
assert patch_result["label"] == "step-64"
assert patch_result["namespace"] == "patch-benchmark"
assert payload_result["count"] == 64
assert payload_result["payload_sum"] == 2016
assert payload_result["payload_span"] == 63
assert context_graph_result is not None
assert context_graph_result["context_graph_items"] > 0
assert context_graph_result["context_graph_digest"]
assert "claim:incident:8" in context_graph_result["context_graph_keys"], context_graph_result["context_graph_keys"]
assert "evidence:incident:8" in context_graph_result["context_graph_keys"], context_graph_result["context_graph_keys"]
assert memo_baseline_result["memo_visits"] == MEMO_BENCHMARK_VISITS
assert memo_hit_result["memo_visits"] == MEMO_BENCHMARK_VISITS
assert memo_baseline_result["memo_output"] == memo_hit_result["memo_output"]
assert memo_baseline_result == memo_hit_result
assert memo_invalidated_result["memo_visits"] == MEMO_BENCHMARK_VISITS
assert memo_invalidated_result["memo_input"] == 7 + MEMO_BENCHMARK_VISITS - 1
assert memo_invalidated_result["memo_output"] == memo_mix(memo_invalidated_result["memo_input"])
assert memo_benchmark_calls["baseline"] == MEMO_BENCHMARK_ITERATIONS * MEMO_BENCHMARK_VISITS
assert memo_benchmark_calls["hit"] == MEMO_BENCHMARK_ITERATIONS
assert memo_benchmark_calls["invalidated"] == MEMO_BENCHMARK_ITERATIONS * MEMO_BENCHMARK_VISITS

assert parallel_details is not None
assert parallel_details["summary"]["status"] == "completed"
assert parallel_details["state"]["parallel_complete"] is True
assert parallel_details["state"]["parallel_total_visits"] == parallel_session_count
for index in range(parallel_session_count):
    assert parallel_details["state"][f"session_{index}_answer"] == (
        f"specialist-{index}::query-{index}::visit-1"
    )
    assert parallel_details["state"][f"session_{index}_visits"] == 1

parallel_session_events = [
    event for event in parallel_details["trace"] if event.get("session_id")
]
parallel_namespaced_events = [event for event in parallel_session_events if event["namespaces"]]
assert len(parallel_session_events) >= parallel_session_count
assert len(parallel_namespaced_events) == parallel_session_count
assert {
    event["session_id"] for event in parallel_session_events
} == {f"specialist-{index}" for index in range(parallel_session_count)}
assert {event["session_revision"] for event in parallel_session_events} == {1}
assert persistent_session_calls["parallel"] == parallel_iterations * parallel_session_count

assert paused_metadata is not None
assert paused_metadata["summary"]["status"] == "paused"
assert paused_metadata["summary"]["checkpoint_id"] > 0
assert direct_resume_details is not None
assert paused_resume_details is not None
assert direct_resume_details["summary"]["status"] == "completed"
assert paused_resume_details["summary"]["status"] == "completed"
assert direct_resume_details["state"]["visits"] == 2
assert paused_resume_details["state"]["visits"] == 2
assert direct_resume_details["state"]["prior_memory"] == 1
assert paused_resume_details["state"]["prior_memory"] == 1
assert direct_resume_details["state"]["memory"] == ["initial-brief", "followup-brief"]
assert paused_resume_details["state"]["memory"] == ["initial-brief", "followup-brief"]
assert direct_resume_details["state"]["memo"] == "memo::resume-specialist"
assert paused_resume_details["state"]["memo"] == "memo::resume-specialist"
assert direct_resume_details["state"]["answer"] == "resume-specialist::followup-brief::visit-2"
assert paused_resume_details["state"]["answer"] == "resume-specialist::followup-brief::visit-2"
assert direct_resume_details["state"] == paused_resume_details["state"]
assert persistent_session_calls["direct"] == resume_iterations
assert persistent_session_calls["resumed"] == resume_iterations

direct_resume_session_events = [
    event for event in direct_resume_details["trace"] if event.get("session_id") == "resume-specialist"
]
paused_resume_session_events = [
    event for event in paused_resume_details["trace"] if event.get("session_id") == "resume-specialist"
]
direct_resume_namespaced_events = [
    event for event in direct_resume_session_events if event["namespaces"]
]
paused_resume_namespaced_events = [
    event for event in paused_resume_session_events if event["namespaces"]
]
assert direct_resume_session_events
assert paused_resume_session_events
assert direct_resume_namespaced_events
assert paused_resume_namespaced_events
assert max(event["session_revision"] for event in direct_resume_session_events) == 2
assert max(event["session_revision"] for event in paused_resume_session_events) == 2

invoke_elapsed_ns = invoke_end_ns - start_ns
record_once_hit_elapsed_ns = record_once_hit_end_ns - record_once_hit_start_ns
record_once_miss_elapsed_ns = record_once_miss_end_ns - record_once_miss_start_ns
patch_elapsed_ns = patch_end_ns - patch_start_ns
payload_elapsed_ns = payload_end_ns - payload_start_ns
context_graph_elapsed_ns = context_graph_end_ns - context_graph_start_ns
memo_baseline_elapsed_ns = memo_baseline_end_ns - memo_baseline_start_ns
memo_hit_elapsed_ns = memo_hit_end_ns - memo_hit_start_ns
memo_invalidated_elapsed_ns = memo_invalidated_end_ns - memo_invalidated_start_ns
parallel_elapsed_ns = parallel_end_ns - parallel_start_ns
resume_direct_elapsed_ns = resume_direct_end_ns - resume_direct_start_ns
resume_paused_elapsed_ns = resume_paused_end_ns - resume_paused_start_ns

direct_resume_digest = direct_resume_details["proof"]["combined_digest"]
paused_resume_digest = paused_resume_details["proof"]["combined_digest"]
parallel_digest = parallel_details["proof"]["combined_digest"]

print(f"python_state_graph_api_invoke_total_ns={invoke_elapsed_ns}")
print(f"python_state_graph_api_invoke_avg_ns={invoke_elapsed_ns // iterations}")
print(f"python_state_graph_api_ainvoke_total_ns={async_elapsed_ns}")
print(f"python_state_graph_api_ainvoke_avg_ns={async_elapsed_ns // async_iterations}")
print(f"python_state_graph_record_once_hit_total_ns={record_once_hit_elapsed_ns}")
print(f"python_state_graph_record_once_hit_avg_ns={record_once_hit_elapsed_ns // iterations}")
print(f"python_state_graph_record_once_hit_producer_calls={record_once_calls['hit']}")
print(f"python_state_graph_record_once_miss_total_ns={record_once_miss_elapsed_ns}")
print(f"python_state_graph_record_once_miss_avg_ns={record_once_miss_elapsed_ns // iterations}")
print(f"python_state_graph_record_once_miss_producer_calls={record_once_calls['miss']}")
print(f"python_state_graph_patch_marshalling_total_ns={patch_elapsed_ns}")
print(
    "python_state_graph_patch_marshalling_avg_ns="
    f"{patch_elapsed_ns // PATCH_MARSHALLING_ITERATIONS}"
)
print(
    "python_state_graph_patch_marshalling_result="
    f"{patch_result['count']}:{patch_result['score']}:{patch_result['label']}"
)
print(f"python_state_graph_payload_materialization_total_ns={payload_elapsed_ns}")
print(
    "python_state_graph_payload_materialization_avg_ns="
    f"{payload_elapsed_ns // PAYLOAD_BENCHMARK_ITERATIONS}"
)
print(
    "python_state_graph_payload_materialization_result="
    f"{payload_result['payload_sum']}:{payload_result['payload_span']}"
)
print(f"python_context_graph_iterations={CONTEXT_GRAPH_BENCHMARK_ITERATIONS}")
print(f"python_context_graph_records={CONTEXT_GRAPH_RECORDS}")
print(f"python_context_graph_total_ns={context_graph_elapsed_ns}")
print(f"python_context_graph_avg_ns={context_graph_elapsed_ns // CONTEXT_GRAPH_BENCHMARK_ITERATIONS}")
print(
    "python_context_graph_cold_view_avg_ns="
    f"{context_graph_timings['cold_ns'] // CONTEXT_GRAPH_BENCHMARK_ITERATIONS}"
)
print(
    "python_context_graph_warm_view_avg_ns="
    f"{context_graph_timings['warm_ns'] // CONTEXT_GRAPH_BENCHMARK_ITERATIONS}"
)
print(
    "python_context_graph_python_rank_avg_ns="
    f"{context_graph_timings['python_rank_ns'] // CONTEXT_GRAPH_BENCHMARK_ITERATIONS}"
)
print(f"python_context_graph_items={context_graph_result['context_graph_items']}")
print(f"python_context_graph_digest={context_graph_result['context_graph_digest']}")
print(f"python_deterministic_memo_iterations={MEMO_BENCHMARK_ITERATIONS}")
print(f"python_deterministic_memo_visits={MEMO_BENCHMARK_VISITS}")
print(f"python_deterministic_memo_rounds={MEMO_BENCHMARK_ROUNDS}")
print(f"python_deterministic_memo_baseline_total_ns={memo_baseline_elapsed_ns}")
print(f"python_deterministic_memo_baseline_avg_ns={memo_baseline_elapsed_ns // MEMO_BENCHMARK_ITERATIONS}")
print(f"python_deterministic_memo_hit_total_ns={memo_hit_elapsed_ns}")
print(f"python_deterministic_memo_hit_avg_ns={memo_hit_elapsed_ns // MEMO_BENCHMARK_ITERATIONS}")
print(
    "python_deterministic_memo_hit_speedup_x="
    f"{(memo_baseline_elapsed_ns / memo_hit_elapsed_ns) if memo_hit_elapsed_ns else 0.0}"
)
print(f"python_deterministic_memo_invalidated_total_ns={memo_invalidated_elapsed_ns}")
print(
    "python_deterministic_memo_invalidated_avg_ns="
    f"{memo_invalidated_elapsed_ns // MEMO_BENCHMARK_ITERATIONS}"
)
print(
    "python_deterministic_memo_invalidation_vs_hit_x="
    f"{(memo_invalidated_elapsed_ns / memo_hit_elapsed_ns) if memo_hit_elapsed_ns else 0.0}"
)
print(
    "python_deterministic_memo_baseline_executor_calls="
    f"{memo_benchmark_calls['baseline']}"
)
print(f"python_deterministic_memo_hit_executor_calls={memo_benchmark_calls['hit']}")
print(
    "python_deterministic_memo_invalidated_executor_calls="
    f"{memo_benchmark_calls['invalidated']}"
)
print(
    "python_deterministic_memo_output_equal="
    f"{int(memo_baseline_result['memo_output'] == memo_hit_result['memo_output'])}"
)
print(f"python_deterministic_memo_invalidated_final_input={memo_invalidated_result['memo_input']}")
print(f"python_persistent_session_parallel_total_ns={parallel_elapsed_ns}")
print(f"python_persistent_session_parallel_avg_ns={parallel_elapsed_ns // parallel_iterations}")
print(f"python_persistent_session_parallel_committed_sessions={parallel_session_count}")
print(f"python_persistent_session_parallel_namespaced_events={len(parallel_namespaced_events)}")
print(f"python_persistent_session_parallel_session_events={len(parallel_session_events)}")
print(f"python_persistent_session_parallel_producer_calls={persistent_session_calls['parallel']}")
print(f"python_persistent_session_parallel_digest={parallel_digest}")
print(f"python_persistent_session_resume_direct_total_ns={resume_direct_elapsed_ns}")
print(f"python_persistent_session_resume_direct_avg_ns={resume_direct_elapsed_ns // resume_iterations}")
print(f"python_persistent_session_resume_total_ns={resume_paused_elapsed_ns}")
print(f"python_persistent_session_resume_avg_ns={resume_paused_elapsed_ns // resume_iterations}")
print(
    f"python_persistent_session_resume_committed_sessions="
    f"{max(event['session_revision'] for event in paused_resume_session_events)}"
)
print(
    f"python_persistent_session_resume_checkpoint_id="
    f"{paused_metadata['summary']['checkpoint_id']}"
)
print(
    f"python_persistent_session_resume_namespaced_events="
    f"{len(paused_resume_namespaced_events)}"
)
print(
    f"python_persistent_session_resume_session_events="
    f"{len(paused_resume_session_events)}"
)
print(
    f"python_persistent_session_resume_direct_producer_calls="
    f"{persistent_session_calls['direct']}"
)
print(
    f"python_persistent_session_resume_resumed_producer_calls="
    f"{persistent_session_calls['resumed']}"
)
print(
    f"python_persistent_session_resume_state_equal="
    f"{int(direct_resume_details['state'] == paused_resume_details['state'])}"
)
print(f"python_persistent_session_resume_direct_digest={direct_resume_digest}")
print(f"python_persistent_session_resume_resumed_digest={paused_resume_digest}")
