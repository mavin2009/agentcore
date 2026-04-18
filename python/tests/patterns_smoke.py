import asyncio

from agentcore import END, START, PipelineGraph, RuntimeContext, SpecialistTeam, StateGraph


async def enrich(state, config):
    await asyncio.sleep(0)
    topic = state["topic"]
    steps = list(state.get("steps", []))
    steps.append(f"enrich::{topic}")
    return {
        "steps": steps,
        "draft": f"draft::{topic}",
    }


def finalize(state, config):
    steps = list(state.get("steps", []))
    steps.append("finalize")
    return {
        "steps": steps,
        "final": f"{state['draft']}::final",
    }


pipeline = PipelineGraph(dict, name="python_pipeline_pattern_smoke", worker_count=2)
pipeline.add_step(
    "seed",
    lambda state, config: {
        "topic": dict(config.get("configurable", {})).get("topic", "unset"),
        "steps": ["seed"],
    },
)
pipeline.add_step("enrich", enrich)
pipeline.add_step("finalize", finalize)
compiled_pipeline = pipeline.compile()

pipeline_result = compiled_pipeline.invoke({}, config={"configurable": {"topic": "native-runtime"}})
assert pipeline_result["topic"] == "native-runtime"
assert pipeline_result["draft"] == "draft::native-runtime"
assert pipeline_result["final"] == "draft::native-runtime::final"
assert pipeline_result["steps"] == ["seed", "enrich::native-runtime", "finalize"]

pipeline_events = list(
    compiled_pipeline.stream({}, config={"configurable": {"topic": "streamed-runtime"}})
)
assert [event["node_name"] for event in pipeline_events] == ["seed", "enrich", "finalize"]


def build_specialist_graph():
    def specialist_step(state, config, runtime: RuntimeContext):
        session_id = str(state.get("session_id", "unknown"))
        query = str(state.get("query", ""))
        memory = list(state.get("memory", []))
        memory.append(query)
        memoized = runtime.record_once(
            f"patterns::{session_id}::memo",
            {"session_id": session_id},
            lambda: {"memo": f"memo::{session_id}"},
        )
        return {
            "answer": f"{session_id}::{query}::visit-{len(memory)}",
            "memory": memory,
            "memo": memoized["memo"],
            "visits": len(memory),
        }

    graph = StateGraph(dict, name="python_pattern_specialist_child", worker_count=2)
    graph.add_node("specialist_step", specialist_step)
    graph.add_edge(START, "specialist_step")
    graph.add_edge("specialist_step", END)
    return graph


specialist_child = build_specialist_graph()

team = SpecialistTeam(dict, name="python_specialist_team_pattern_smoke", worker_count=4)
team.set_dispatch(
    "dispatch",
    lambda state, config: {"topic": dict(config.get("configurable", {})).get("topic", "unset")},
)
team.add_specialist(
    "planner",
    specialist_child,
    prepare=lambda state, config: {
        "planner_session_id": "planner",
        "planner_query": f"plan::{state['topic']}",
    },
    inputs={
        "planner_session_id": "session_id",
        "planner_query": "query",
    },
    outputs={
        "planner_answer": "answer",
        "planner_memory": "memory",
        "planner_memo": "memo",
        "planner_visits": "visits",
    },
)
team.add_specialist(
    "critic",
    specialist_child,
    prepare=lambda state, config: {
        "critic_session_id": "critic",
        "critic_query": f"critique::{state['topic']}",
    },
    inputs={
        "critic_session_id": "session_id",
        "critic_query": "query",
    },
    outputs={
        "critic_answer": "answer",
        "critic_memory": "memory",
        "critic_memo": "memo",
        "critic_visits": "visits",
    },
)
team.set_aggregate(
    "synthesize",
    lambda state, config: {
        "team_summary": f"{state['planner_answer']}|{state['critic_answer']}",
        "complete": True,
    },
)
compiled_team = team.compile()

team_details = compiled_team.invoke_with_metadata(
    {},
    config={"configurable": {"topic": "scheduler-hardening"}, "tags": ["patterns", "team"]},
)
assert team_details["summary"]["status"] == "completed"
assert team_details["state"]["planner_answer"] == "planner::plan::scheduler-hardening::visit-1"
assert team_details["state"]["critic_answer"] == "critic::critique::scheduler-hardening::visit-1"
assert team_details["state"]["planner_memory"] == ["plan::scheduler-hardening"]
assert team_details["state"]["critic_memory"] == ["critique::scheduler-hardening"]
assert team_details["state"]["planner_memo"] == "memo::planner"
assert team_details["state"]["critic_memo"] == "memo::critic"
assert team_details["state"]["planner_visits"] == 1
assert team_details["state"]["critic_visits"] == 1
assert team_details["state"]["team_summary"] == (
    "planner::plan::scheduler-hardening::visit-1|"
    "critic::critique::scheduler-hardening::visit-1"
)
assert team_details["state"]["complete"] is True

team_events = [event for event in team_details["trace"] if event["namespaces"]]
assert {event["session_id"] for event in team_events} == {"planner", "critic"}
assert {event["session_revision"] for event in team_events} == {1}
assert {
    event["namespaces"][0]["node_name"]
    for event in team_events
} == {"planner", "critic"}

streamed_team_events = list(
    compiled_team.stream(
        {},
        config={"configurable": {"topic": "stream-patterns"}, "tags": ["patterns", "stream"]},
    )
)
streamed_namespaced_events = [event for event in streamed_team_events if event["namespaces"]]
assert {event["session_id"] for event in streamed_namespaced_events} == {"planner", "critic"}
assert all(event["namespaces"][0]["session_id"] in {"planner", "critic"} for event in streamed_namespaced_events)

print("python patterns smoke passed")
