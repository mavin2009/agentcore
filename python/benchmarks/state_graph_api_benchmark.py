import asyncio
import time

from agentcore.graph import Command, END, START, RuntimeContext, StateGraph


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

record_once_calls = {"miss": 0, "hit": 0}


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
print(
    f"python_persistent_session_resume_proof_equal="
    f"{int(direct_resume_digest == paused_resume_digest)}"
)
print(f"python_persistent_session_resume_direct_digest={direct_resume_digest}")
print(f"python_persistent_session_resume_resumed_digest={paused_resume_digest}")
